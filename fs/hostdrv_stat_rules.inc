/* ========================================================================
 *  hostdrv_stat_rules.inc — hdrv_stat() の「成功と言ってよいか」の純規則
 *
 *  票: H1 (docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md §2 / §4.3)
 *  ホスト試験: tools/tests/hsync_h1_host.c (A11)
 *
 *  直した点:
 *    1. Basic / Standard 情報の問い合わせに失敗しても最後に成功を返す経路が
 *       あった。取れなかった値は kmemset 由来のゼロのままなので、
 *       **実在する空ファイル / 種別不明** と見分けが付かず、hsync の同一判定
 *       (サイズ 0 同士) をすり抜ける。取れなければ I/O エラーを返す。
 *    2. NT の EndOfFile は 64bit。u32 の st_size へ**黙って切り詰めない**。
 *       4GiB を越えるものは表現できないと言って断る (設計書 §4.3)。
 *
 *  時刻 (LastWriteTime) の転記は票 H3。ここでは触らない。
 *
 *  ハイパーコールにもカーネルの他のヘッダにも触らない純関数なので、
 *  ホストでそのまま走らせて試験できる。
 *
 *  [C1] C89 / GNU89。宣言はブロック先頭、`//` コメント無し。
 * ======================================================================== */

#ifndef FS_HOSTDRV_STAT_RULES_INC
#define FS_HOSTDRV_STAT_RULES_INC

/* FILE_ATTRIBUTE_DIRECTORY。hostdrvfs.c 側で NP2_FILE_ATTRIBUTE_DIRECTORY
 * と一致することを STATIC_ASSERT で固定している ([C4])。 */
#ifndef HDRV_STAT_ATTR_DIRECTORY
#define HDRV_STAT_ATTR_DIRECTORY 0x00000010UL
#endif

/* st_size (u32) で表せる上限。これを越える EndOfFile は切り詰めずに断る。 */
#ifndef HDRV_STAT_MAX_SIZE
#define HDRV_STAT_MAX_SIZE 0xFFFFFFFFULL
#endif

/* 問い合わせ結果から OS32_Stat を組む。
 *
 *   basic_rc / std_rc … hostdrv_query_info() の戻り値 (0 = 成功)
 *   attributes        … Np2FileBasicInfo.FileAttributes
 *   end_of_file       … Np2FileStandardInfo.EndOfFile (64bit のまま渡す)
 *
 * 戻り値 0 = 成功 (buf を埋めた) / OS32_ERR_IO = 成功と言えない。
 * buf のゼロ初期化は呼び手の責任 (失敗時は buf を信用しないこと)。 */
static int hdrv_stat_fill(int basic_rc, u32 attributes,
                          int std_rc, unsigned long long end_of_file,
                          OS32_Stat *buf)
{
    if (!buf) return OS32_ERR_IO;

    /* 種別が分からない。ゼロの st_mode を「通常ファイル」と読ませない */
    if (basic_rc != 0) return OS32_ERR_IO;
    /* サイズが分からない。ゼロを「空ファイル」と読ませない */
    if (std_rc != 0) return OS32_ERR_IO;
    /* 64bit -> u32 の切り詰めは検出して断る (設計書 §4.3) */
    if (end_of_file > HDRV_STAT_MAX_SIZE) return OS32_ERR_IO;

    if (attributes & HDRV_STAT_ATTR_DIRECTORY) {
        buf->st_mode = (u16)(OS_S_IFDIR | 0755);
    } else {
        buf->st_mode = (u16)(OS_S_IFREG | 0644);
    }
    buf->st_size = (u32)end_of_file;
    return 0;
}

#endif /* FS_HOSTDRV_STAT_RULES_INC */
