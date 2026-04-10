/* ======================================================================== */
/*  IME_DICT.C — FEP辞書検索エンジン                                        */
/*                                                                          */
/*  2階層インデックス検索 + 16KB FIFOブロックキャッシュ                      */
/*  カーネルVFS (vfs_open/read_fd/seek) を直接使用                           */
/* ======================================================================== */

#include "ime.h"
#include "vfs.h"
#include "kmalloc.h"
#include "utf8.h"
#include "kprintf.h"
#include "kstring.h"
#include "os32_kapi_shared.h"

/* ======================================================================== */
/*  検索ヘルパー                                                             */
/* ======================================================================== */

/* Level-1 キー照合 (u32 等価比較) */
static int match_l1_key(const u8 *key4, const u8 *utf8_str)
{
    return *(const u32 *)key4 == utf8_pack32(utf8_str);
}

/* Level-2 二分探索 */
static int bsearch_l2(const u8 *key8, const IME_L2Entry *base, int count)
{
    int left = 0;
    int right = count - 1;
    int mid, cmp;
    u32 ta, tb, ea, eb;

    ta = *(const u32 *)key8;
    tb = *(const u32 *)(key8 + 4);

    while (left <= right) {
        mid = left + (right - left) / 2;
        ea = *(const u32 *)base[mid].key_chars;
        eb = *(const u32 *)(base[mid].key_chars + 4);

        cmp = utf8_cmp32(ta, ea);
        if (cmp == 0) cmp = utf8_cmp32(tb, eb);

        if (cmp == 0)  return mid;
        if (cmp < 0)   right = mid - 1;
        else           left = mid + 1;
    }
    return -1;
}

/* UTF-8 完全一致チェック
 * text (辞書のyomi, text_len バイト) と prefix (入力yomi, NUL終端)
 * がバイト列として完全一致する場合のみ 1 を返す。
 */
static int utf8_exact_match(const u8 *text, int text_len, const u8 *prefix)
{
    int plen = (int)kstrlen((const char *)prefix);
    int i;

    if (plen != text_len) return 0;

    for (i = 0; i < plen; i++) {
        if (text[i] != prefix[i]) return 0;
    }
    return 1;
}

/* コスト昇順で挿入ソート */
static void sort_results(IME_Result *res, int count)
{
    int i, j;
    IME_Result tmp;
    for (i = 1; i < count; i++) {
        kmemcpy(&tmp, &res[i], sizeof(IME_Result));
        j = i - 1;
        while (j >= 0 && res[j].cost > tmp.cost) {
            kmemcpy(&res[j + 1], &res[j], sizeof(IME_Result));
            j--;
        }
        kmemcpy(&res[j + 1], &tmp, sizeof(IME_Result));
    }
}

/* ======================================================================== */
/*  16KB FIFOブロックキャッシュ                                              */
/* ======================================================================== */

static u8 *cache_find(IME_Dict *dict, const u8 *l2_key, u32 *out_size)
{
    u32 pos = 0;
    while (pos + IME_CACHE_HDR_SIZE <= dict->cache_used) {
        IME_CacheHdr *hdr = (IME_CacheHdr *)(dict->cache_buf + pos);
        u32 entry_size = IME_CACHE_HDR_SIZE + hdr->block_size;

        if (*(const u32 *)hdr->l2_key == *(const u32 *)l2_key &&
            *(const u32 *)(hdr->l2_key + 4) == *(const u32 *)(l2_key + 4)) {
            *out_size = hdr->block_size;
            return dict->cache_buf + pos + IME_CACHE_HDR_SIZE;
        }
        pos += entry_size;
    }
    return NULL;
}

static void cache_add(IME_Dict *dict, const u8 *l2_key,
                      const u8 *data, u32 data_size)
{
    u32 need = IME_CACHE_HDR_SIZE + data_size;
    IME_CacheHdr hdr;

    if (need > IME_CACHE_SIZE) return;

    /* FIFO淘汰: 先頭(最古)から必要分を破棄 */
    while (dict->cache_used + need > IME_CACHE_SIZE && dict->cache_used > 0) {
        IME_CacheHdr *oldest = (IME_CacheHdr *)dict->cache_buf;
        u32 evict_size = IME_CACHE_HDR_SIZE + oldest->block_size;
        if (evict_size > dict->cache_used) {
            dict->cache_used = 0;
            break;
        }
        kmemcpy(dict->cache_buf, dict->cache_buf + evict_size,
                dict->cache_used - evict_size);
        dict->cache_used -= evict_size;
    }

    kmemcpy(hdr.l2_key, l2_key, 8);
    hdr.block_size = data_size;
    kmemcpy(dict->cache_buf + dict->cache_used, &hdr, IME_CACHE_HDR_SIZE);
    kmemcpy(dict->cache_buf + dict->cache_used + IME_CACHE_HDR_SIZE,
            data, data_size);
    dict->cache_used += need;
}

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

int ime_dict_open(IME_Dict *dict, const char *path)
{
    int fd, bytes_read;
    u32 l1_size, l2_size, i;

    kmemset(dict, 0, sizeof(IME_Dict));
    kstrncpy(dict->dict_path, path, 127);
    dict->dict_path[127] = '\0';

    fd = vfs_open(path, KAPI_O_RDONLY);
    if (fd < 0) {
        kprintf(ATTR_RED, "IME: Failed to open %s\r\n", path);
        return -1;
    }

    /* ヘッダ (16B) */
    bytes_read = vfs_read_fd(fd, (void *)&dict->header, sizeof(IME_DictHeader));
    if (bytes_read != (int)sizeof(IME_DictHeader)) {
        kprintf(ATTR_RED, "IME: Header read error\r\n");
        vfs_close(fd);
        return -2;
    }

    if (dict->header.magic != IME_DICT_MAGIC) {
        kprintf(ATTR_RED, "IME: Invalid magic 0x%x\r\n", dict->header.magic);
        vfs_close(fd);
        return -3;
    }
    if (dict->header.version != IME_DICT_VERSION) {
        kprintf(ATTR_RED, "IME: Bad version %d\r\n", dict->header.version);
        vfs_close(fd);
        return -4;
    }

    /* Level-1 */
    l1_size = dict->header.l1_count * sizeof(IME_L1Entry);
    dict->l1_index = (IME_L1Entry *)kmalloc(l1_size);
    if (!dict->l1_index) {
        kprintf(ATTR_RED, "IME: L1 alloc fail\r\n");
        vfs_close(fd);
        return -5;
    }
    bytes_read = vfs_read_fd(fd, (void *)dict->l1_index, l1_size);
    if ((u32)bytes_read != l1_size) {
        kprintf(ATTR_RED, "IME: L1 read error\r\n");
        vfs_close(fd);
        return -6;
    }

    /* Level-2 総エントリ数 */
    dict->l2_total = 0;
    for (i = 0; i < dict->header.l1_count; i++) {
        dict->l2_total += dict->l1_index[i].l2_count;
    }

    /* Level-2 */
    l2_size = dict->l2_total * sizeof(IME_L2Entry);
    dict->l2_index = (IME_L2Entry *)kmalloc(l2_size);
    if (!dict->l2_index) {
        kprintf(ATTR_RED, "IME: L2 alloc fail\r\n");
        vfs_close(fd);
        return -7;
    }
    bytes_read = vfs_read_fd(fd, (void *)dict->l2_index, l2_size);
    if ((u32)bytes_read != l2_size) {
        kprintf(ATTR_RED, "IME: L2 read error\r\n");
        vfs_close(fd);
        return -8;
    }

    vfs_close(fd);

    /* ブロックバッファ */
    dict->block_buf_size = IME_BLOCK_BUF_SIZE;
    dict->block_buf = (u8 *)kmalloc(dict->block_buf_size);
    if (!dict->block_buf) {
        kprintf(ATTR_RED, "IME: block buf alloc fail\r\n");
        return -9;
    }

    /* キャッシュバッファ */
    dict->cache_buf = (u8 *)kmalloc(IME_CACHE_SIZE);
    if (!dict->cache_buf) {
        kprintf(ATTR_RED, "IME: cache alloc fail\r\n");
        return -10;
    }
    dict->cache_used = 0;

    kprintf(ATTR_GREEN, "IME: Dict loaded: %d words, L1=%d, L2=%d\r\n",
            dict->header.total_words, dict->header.l1_count, dict->l2_total);
    return 0;
}

int ime_dict_search(IME_Dict *dict, const char *yomi,
                    IME_Result *results, int max_results)
{
    u32 i;
    int l1_found;
    IME_L1Entry *l1e;
    IME_L2Entry *l2_sub;
    int l2_sub_count;
    u8 l2_key[8];
    int l2_idx, l2_global_idx;
    u32 block_offset, block_size;
    int bytes_read;
    u32 pos;
    int result_count;
    int yomi_first_len, yomi_second_len;
    u8 *block_data;
    u32 block_data_size;

    if (!dict || !yomi || !results || max_results <= 0) return 0;
    if (yomi[0] == '\0') return 0;

    /* Level-1 線形探索 */
    l1_found = -1;
    for (i = 0; i < dict->header.l1_count; i++) {
        if (match_l1_key(dict->l1_index[i].key_char, (const u8 *)yomi)) {
            l1_found = (int)i;
            break;
        }
    }
    if (l1_found < 0) return 0;
    l1e = &dict->l1_index[l1_found];

    /* Level-2 サブインデックス特定 */
    {
        u32 l2_mem_start = sizeof(IME_DictHeader)
                         + dict->header.l1_count * sizeof(IME_L1Entry);
        u32 l2_sub_index = (l1e->l2_offset - l2_mem_start) / sizeof(IME_L2Entry);
        l2_sub = &dict->l2_index[l2_sub_index];
        l2_sub_count = (int)l1e->l2_count;
    }

    /* L2キー構築 + 二分探索 */
    kmemset(l2_key, 0, 8);
    yomi_first_len = utf8_char_bytes((const u8 *)yomi);
    if (yomi[yomi_first_len] != '\0') {
        yomi_second_len = utf8_char_bytes((const u8 *)yomi + yomi_first_len);
        kmemcpy(l2_key, yomi, yomi_first_len + yomi_second_len);
    } else {
        kmemcpy(l2_key, yomi, yomi_first_len);
    }

    l2_idx = bsearch_l2(l2_key, l2_sub, l2_sub_count);
    if (l2_idx < 0) return 0;

    l2_global_idx = (int)(l2_sub - dict->l2_index) + l2_idx;

    /* ブロックサイズ算出 */
    block_offset = dict->l2_index[l2_global_idx].data_offset;
    if ((u32)(l2_global_idx + 1) < dict->l2_total) {
        block_size = dict->l2_index[l2_global_idx + 1].data_offset - block_offset;
    } else {
        block_size = dict->block_buf_size;
    }
    if (block_size > dict->block_buf_size) {
        block_size = dict->block_buf_size;
    }

    /* キャッシュ検索 → ミス時はディスクI/O */
    block_data = cache_find(dict, l2_key, &block_data_size);
    if (block_data) {
        bytes_read = (int)block_data_size;
    } else {
        int fd = vfs_open(dict->dict_path, KAPI_O_RDONLY);
        if (fd < 0) return -1;

        vfs_seek(fd, (int)block_offset, SEEK_SET);
        bytes_read = vfs_read_fd(fd, (void *)dict->block_buf, block_size);
        vfs_close(fd);

        if (bytes_read <= 0) return 0;

        block_data = dict->block_buf;
        block_data_size = (u32)bytes_read;
        cache_add(dict, l2_key, block_data, block_data_size);
    }

    /* ブロック走査 (前方一致) */
    result_count = 0;
    pos = 0;
    while (pos + 4 <= (u32)bytes_read && result_count < max_results) {
        u32 meta;
        int y_len, k_len;
        u16 p_id, c_val;

        kmemcpy(&meta, &block_data[pos], 4);
        pos += 4;

        y_len = (int)WMETA_YOMI_LEN(meta);
        k_len = (int)WMETA_KANJI_LEN(meta);
        p_id  = (u16)WMETA_POS_ID(meta);
        c_val = (u16)WMETA_COST(meta);

        if (pos + y_len + k_len > (u32)bytes_read) break;

        if (utf8_exact_match(&block_data[pos], y_len, (const u8 *)yomi)) {
            int copy_len;
            IME_Result *r = &results[result_count];

            copy_len = y_len < 31 ? y_len : 31;
            kmemcpy(r->yomi, &block_data[pos], copy_len);
            r->yomi[copy_len] = '\0';

            copy_len = k_len < 31 ? k_len : 31;
            kmemcpy(r->kanji, &block_data[pos + y_len], copy_len);
            r->kanji[copy_len] = '\0';

            r->pos_id = p_id;
            r->cost = c_val;
            result_count++;
        }
        pos += y_len + k_len;
    }

    if (result_count > 1) {
        sort_results(results, result_count);
    }
    return result_count;
}
