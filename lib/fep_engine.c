/* ======================================================================== */
/*  FEP_ENGINE.C - OS32 FEP 辞書検索エンジン                                 */
/*                                                                          */
/*  .dic バイナリ辞書のインデックス読み込み・検索エンジンの実装。             */
/*  2階層インデックス (Level-1: 先頭1文字, Level-2: 先頭2文字) を            */
/*  起動時にオンメモリ常駐させ、検索時はデータブロックのみディスクI/Oする。   */
/*                                                                          */
/*  UTF-8比較はカーネル lib/utf8.c の u32 最適化プリミティブを使用。          */
/*  i386の32bitワード読み出しでバイト比較より高速。                           */
/* ======================================================================== */

#include "os32api.h"
#include "fep_engine.h"
#include "utf8.h"
#include <string.h>

/* ======================================================================== */
/*  内部ユーティリティ                                                       */
/* ======================================================================== */

/*
 * Level-1 キー照合 (u32 等価比較)
 *
 * L1 key_char は 4B でゼロパディング済み。
 * utf8_pack32() も同じくゼロパディングした u32 を返すので、
 * 単純な u32 等価比較で照合できる。
 */
static int match_l1_key(const u8 *key4, const u8 *utf8_str)
{
    return *(const u32 *)key4 == utf8_pack32(utf8_str);
}

/*
 * Level-2 インデックスを二分探索する (u32 辞書順比較)
 *
 * L2 key_chars は 8B (先頭2文字)。
 * 前半4Bと後半4Bをそれぞれ u32 として辞書順比較する。
 *
 * key8_target: 検索対象の先頭2文字キー (8B)
 * l2_base: Level-2 サブインデックスの先頭
 * l2_count: エントリ数
 * 戻り値: 見つかったエントリのインデックス, -1=見つからない
 */
static int bsearch_l2(const u8 *key8_target, const FEP_L2Entry *l2_base, int l2_count)
{
    int left = 0;
    int right = l2_count - 1;
    int mid, cmp;
    u32 ta, tb, ea, eb;

    /* ターゲットキーの前半・後半を事前にパック */
    ta = *(const u32 *)key8_target;
    tb = *(const u32 *)(key8_target + 4);

    while (left <= right) {
        mid = left + (right - left) / 2;

        /* エントリキーの前半・後半 */
        ea = *(const u32 *)l2_base[mid].key_chars;
        eb = *(const u32 *)(l2_base[mid].key_chars + 4);

        /* 前半4Bで辞書順比較 */
        cmp = utf8_cmp32(ta, ea);
        if (cmp == 0) {
            /* 前半一致 → 後半で比較 */
            cmp = utf8_cmp32(tb, eb);
        }

        if (cmp == 0)  return mid;
        if (cmp < 0)   right = mid - 1;
        else           left = mid + 1;
    }
    return -1;
}

/*
 * UTF-8文字列の前方一致チェック (u32 単位比較)
 *
 * text: 対象文字列 (長さ text_len)
 * prefix: プレフィックス (ヌル終端)
 * 戻り値: 1=前方一致, 0=不一致
 *
 * 3バイトのひらがな/カタカナ文字を u32 単位で比較することで、
 * バイト比較の1/3の回数で照合できる。
 */
static int utf8_prefix_match(const u8 *text, int text_len, const u8 *prefix)
{
    int plen = strlen((const char *)prefix);
    int t_pos = 0;
    int p_pos = 0;
    int t_cb, p_cb;

    if (plen > text_len) return 0;

    while (p_pos < plen) {
        if (t_pos >= text_len) return 0;

        p_cb = utf8_char_bytes(prefix + p_pos);
        t_cb = utf8_char_bytes(text + t_pos);

        /* バイト長が異なれば文字も異なる */
        if (p_cb != t_cb) return 0;

        /* u32 パック比較 (バッファ末尾保護: 残りが4B未満ならバイト比較) */
        if (t_pos + 4 <= text_len && p_pos + 4 <= plen) {
            if (utf8_pack32(text + t_pos) != utf8_pack32(prefix + p_pos)) {
                return 0;
            }
        } else {
            /* 末尾付近: バイト単位フォールバック */
            int k;
            for (k = 0; k < p_cb; k++) {
                if (text[t_pos + k] != prefix[p_pos + k]) return 0;
            }
        }

        t_pos += t_cb;
        p_pos += p_cb;
    }
    return 1;
}

/* 結果配列をcostの昇順 (小さいほど優先) で挿入ソートする */
static void sort_results_by_cost(FEP_Result *results, int count)
{
    int i, j;
    FEP_Result tmp;
    for (i = 1; i < count; i++) {
        memcpy(&tmp, &results[i], sizeof(FEP_Result));
        j = i - 1;
        while (j >= 0 && results[j].cost > tmp.cost) {
            memcpy(&results[j + 1], &results[j], sizeof(FEP_Result));
            j--;
        }
        memcpy(&results[j + 1], &tmp, sizeof(FEP_Result));
    }
}

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

int fep_dict_open(FEP_Dict *dict, const char *path, KernelAPI *api)
{
    int fd;
    int bytes_read;
    u32 l1_size, l2_size;
    u32 i;

    memset(dict, 0, sizeof(FEP_Dict));
    dict->api = api;

    /* パスを保存 */
    strncpy(dict->dict_path, path, OS32_MAX_PATH - 1);
    dict->dict_path[OS32_MAX_PATH - 1] = '\0';

    /* ファイルを開く */
    fd = api->sys_open(path, KAPI_O_RDONLY);
    if (fd < 0) {
        api->kprintf(ATTR_RED, "FEP: Failed to open %s\r\n", path);
        return -1;
    }

    /* ヘッダ読み込み (16B) */
    bytes_read = api->sys_read(fd, (void *)&dict->header, sizeof(FEP_DictHeader));
    if (bytes_read != sizeof(FEP_DictHeader)) {
        api->kprintf(ATTR_RED, "FEP: Header read error\r\n");
        api->sys_close(fd);
        return -2;
    }

    /* マジックナンバー検証 */
    if (dict->header.magic != FEP_DICT_MAGIC) {
        api->kprintf(ATTR_RED, "FEP: Invalid magic: 0x%x\r\n", dict->header.magic);
        api->sys_close(fd);
        return -3;
    }

    /* バージョン検証 */
    if (dict->header.version != FEP_DICT_VERSION) {
        api->kprintf(ATTR_RED, "FEP: Unsupported version: %d\r\n", dict->header.version);
        api->sys_close(fd);
        return -4;
    }

    /* Level-1 インデックス読み込み */
    l1_size = dict->header.l1_count * sizeof(FEP_L1Entry);
    dict->l1_index = (FEP_L1Entry *)api->mem_alloc(l1_size);
    if (!dict->l1_index) {
        api->kprintf(ATTR_RED, "FEP: Failed to alloc L1 (%d bytes)\r\n", l1_size);
        api->sys_close(fd);
        return -5;
    }

    bytes_read = api->sys_read(fd, (void *)dict->l1_index, l1_size);
    if ((u32)bytes_read != l1_size) {
        api->kprintf(ATTR_RED, "FEP: L1 read error\r\n");
        api->sys_close(fd);
        fep_dict_close(dict);
        return -6;
    }

    /* Level-2 総エントリ数を算出 (全L1の l2_count を合計) */
    dict->l2_total = 0;
    for (i = 0; i < dict->header.l1_count; i++) {
        dict->l2_total += dict->l1_index[i].l2_count;
    }

    /* Level-2 インデックス読み込み */
    l2_size = dict->l2_total * sizeof(FEP_L2Entry);
    dict->l2_index = (FEP_L2Entry *)api->mem_alloc(l2_size);
    if (!dict->l2_index) {
        api->kprintf(ATTR_RED, "FEP: Failed to alloc L2 (%d bytes)\r\n", l2_size);
        api->sys_close(fd);
        fep_dict_close(dict);
        return -7;
    }

    bytes_read = api->sys_read(fd, (void *)dict->l2_index, l2_size);
    if ((u32)bytes_read != l2_size) {
        api->kprintf(ATTR_RED, "FEP: L2 read error\r\n");
        api->sys_close(fd);
        fep_dict_close(dict);
        return -8;
    }

    api->sys_close(fd);

    /* データブロック読み込みバッファの確保 */
    dict->block_buf_size = FEP_BLOCK_BUF_SIZE;
    dict->block_buf = (u8 *)api->mem_alloc(dict->block_buf_size);
    if (!dict->block_buf) {
        api->kprintf(ATTR_RED, "FEP: Failed to alloc block buf\r\n");
        fep_dict_close(dict);
        return -9;
    }

    return 0;
}

void fep_dict_close(FEP_Dict *dict)
{
    if (!dict || !dict->api) return;

    if (dict->l1_index) {
        dict->api->mem_free(dict->l1_index);
        dict->l1_index = NULL;
    }
    if (dict->l2_index) {
        dict->api->mem_free(dict->l2_index);
        dict->l2_index = NULL;
    }
    if (dict->block_buf) {
        dict->api->mem_free(dict->block_buf);
        dict->block_buf = NULL;
    }
}

int fep_dict_search(FEP_Dict *dict, const char *yomi,
                    FEP_Result *results, int max_results)
{
    u32 i;
    int l1_found;
    FEP_L1Entry *l1e;
    FEP_L2Entry *l2_sub;
    int l2_sub_count;
    u8 l2_key[8];
    int l2_idx;
    int l2_global_idx;
    u32 block_offset, block_size;
    int fd, bytes_read;
    u32 pos;
    int result_count;
    int yomi_first_len, yomi_second_len;

    if (!dict || !yomi || !results || max_results <= 0) return 0;
    if (yomi[0] == '\0') return 0;

    /* ---- Step 1: Level-1 検索 (線形探索, u32等価比較) ---- */
    l1_found = -1;
    for (i = 0; i < dict->header.l1_count; i++) {
        if (match_l1_key(dict->l1_index[i].key_char, (const u8 *)yomi)) {
            l1_found = (int)i;
            break;
        }
    }
    if (l1_found < 0) return 0;

    l1e = &dict->l1_index[l1_found];

    /* ---- Step 2: Level-2 サブインデックスの特定 ---- */
    {
        u32 l2_mem_start;
        u32 l2_entry_file_offset;
        u32 l2_sub_index;

        l2_mem_start = sizeof(FEP_DictHeader)
                     + dict->header.l1_count * sizeof(FEP_L1Entry);

        l2_entry_file_offset = l1e->l2_offset;
        l2_sub_index = (l2_entry_file_offset - l2_mem_start) / sizeof(FEP_L2Entry);

        l2_sub = &dict->l2_index[l2_sub_index];
        l2_sub_count = (int)l1e->l2_count;
    }

    /* ---- Step 3: Level-2 二分探索 (u32辞書順比較) ---- */
    memset(l2_key, 0, 8);
    yomi_first_len = utf8_char_bytes((const u8 *)yomi);
    if (yomi[yomi_first_len] != '\0') {
        yomi_second_len = utf8_char_bytes((const u8 *)yomi + yomi_first_len);
        memcpy(l2_key, yomi, yomi_first_len + yomi_second_len);
    } else {
        memcpy(l2_key, yomi, yomi_first_len);
    }

    l2_idx = bsearch_l2(l2_key, l2_sub, l2_sub_count);
    if (l2_idx < 0) return 0;

    /* グローバルL2インデックスを算出 */
    l2_global_idx = (int)(l2_sub - dict->l2_index) + l2_idx;

    /* ---- Step 4: データブロックのサイズ算出 ---- */
    block_offset = dict->l2_index[l2_global_idx].data_offset;
    if ((u32)(l2_global_idx + 1) < dict->l2_total) {
        block_size = dict->l2_index[l2_global_idx + 1].data_offset - block_offset;
    } else {
        block_size = dict->block_buf_size;
    }

    if (block_size > dict->block_buf_size) {
        block_size = dict->block_buf_size;
    }

    /* ---- Step 5: データブロックをディスクからロード ---- */
    fd = dict->api->sys_open(dict->dict_path, KAPI_O_RDONLY);
    if (fd < 0) return -1;

    dict->api->sys_lseek(fd, (int)block_offset, SEEK_SET);
    bytes_read = dict->api->sys_read(fd, (void *)dict->block_buf, block_size);
    dict->api->sys_close(fd);

    if (bytes_read <= 0) return 0;

    /* ---- Step 6: ブロック内シーケンシャル走査 (前方一致, u32比較) ---- */
    result_count = 0;
    pos = 0;

    while (pos + 4 <= (u32)bytes_read && result_count < max_results) {
        u32 meta;
        int y_len, k_len;
        u16 p_id, c_val;

        /* WordMeta32 を読む */
        memcpy(&meta, &dict->block_buf[pos], 4);
        pos += 4;

        y_len = (int)WMETA_YOMI_LEN(meta);
        k_len = (int)WMETA_KANJI_LEN(meta);
        p_id  = (u16)WMETA_POS_ID(meta);
        c_val = (u16)WMETA_COST(meta);

        /* データ境界チェック */
        if (pos + y_len + k_len > (u32)bytes_read) break;

        /* 前方一致チェック (u32最適化) */
        if (utf8_prefix_match(&dict->block_buf[pos], y_len, (const u8 *)yomi)) {
            int copy_len;
            FEP_Result *r = &results[result_count];

            copy_len = y_len < 31 ? y_len : 31;
            memcpy(r->yomi, &dict->block_buf[pos], copy_len);
            r->yomi[copy_len] = '\0';

            copy_len = k_len < 31 ? k_len : 31;
            memcpy(r->kanji, &dict->block_buf[pos + y_len], copy_len);
            r->kanji[copy_len] = '\0';

            r->pos_id = p_id;
            r->cost = c_val;
            result_count++;
        }

        pos += y_len + k_len;
    }

    /* コスト順にソート */
    if (result_count > 1) {
        sort_results_by_cost(results, result_count);
    }

    return result_count;
}
