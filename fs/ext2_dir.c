#include "ext2_priv.h"

/*  ディレクトリ操作 — g_aux使用                                            */
/* ======================================================================== */

int ext2_list_dir(Ext2Ctx *ctx, u32 dir_ino, ext2_dir_callback cb, void *user_ctx)
{
    Ext2Inode inode;
    int ret;
    u32 bi, pos, phys;
    /* 走査中のディレクトリブロックはローカルに写す。コールバックの中で
     * 呼び出し側がファイルへ書く (例: `ls > file` — printf がリダイレクト先の
     * ext2_write_stream を呼ぶ) と、その経路の ext2_bmap / alloc_block が
     * 共有バッファ ext2_g_aux を上書きし、2 件目以降のエントリがビットマップ
     * の中身 (0xFF...) に化けていた (2026-09-03 実測)。 */
    u8 blk[EXT2_BLOCK_SIZE];

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;
    if (!(inode.mode & EXT2_S_IFDIR)) return EXT2_ERR_NOTDIR;

    for (bi = 0; ; bi++) {
        /* 「読めなかった」を「ここで終わり」と読み替えない (票 B8) */
        ret = ext2_bmap(ctx, &inode, bi, &phys);
        if (ret != 0) return EXT2_ERR_IO;
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, blk);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&blk[pos];
            u16 de_reclen = *(u16 *)&blk[pos + 4];
            u8  de_namelen = blk[pos + 6];
            u8  de_type    = blk[pos + 7];

            if (de_reclen == 0) break;

            if (de_inode != 0 && de_namelen > 0) {
                Ext2DirEntry entry;
                int j;
                entry.inode = de_inode;
                entry.rec_len = de_reclen;
                entry.name_len = de_namelen;
                entry.file_type = de_type;
                for (j = 0; j < (int)de_namelen && j < EXT2_NAME_LEN; j++) {
                    entry.name[j] = (char)blk[pos + 8 + j];
                }
                entry.name[j] = '\0';
                cb(&entry, user_ctx);
            }
            pos += de_reclen;
        }
    }
    return EXT2_OK;
}

int ext2_find_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name, u32 *out_ino, u8 *out_type)
{
    Ext2Inode inode;
    int ret, name_len;
    u32 bi, pos, phys;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    name_len = ext2_str_len(name);

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        /* **ここが B8 の入口**。間接ブロックが読めなかったのを「未割当 =
         * 検索終了」と混同すると、実在する名前に NOTFOUND を返す。 */
        ret = ext2_bmap(ctx, &inode, bi, &phys);
        if (ret != 0) return EXT2_ERR_IO;
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];
            u8  de_type    = ext2_g_aux[pos + 7];

            if (de_reclen == 0) break;

            if (de_inode != 0 && de_namelen == (u8)name_len) {
                if (ext2_str_ncmp(name, (const char *)&ext2_g_aux[pos + 8], name_len) == 0) {
                    if (out_ino) *out_ino = de_inode;
                    if (out_type) *out_type = de_type;
                    return EXT2_OK;
                }
            }
            pos += de_reclen;
        }
    }
    return EXT2_ERR_NOTFOUND;
}

/* [a, b) のバイト列が 1 セクタに収まるか (= 1 回の書き込みで原子的に届くか) */
static int ext2_same_sector(u32 a, u32 b)
{
    return (a / EXT2_SECTOR_SIZE) == ((b - 1) / EXT2_SECTOR_SIZE);
}

/* ---- ディレクトリエントリの追加 (票 B8 往復 4) ----------------------------
 *
 * 守る不変条件は往復 3 と同じものの inode 側:
 *
 *     **媒体上のどの名前も、解放済みの inode や、中身の無いブロックを指さない。**
 *
 * 見える状態を変える書き込み (名前が現れる瞬間) を**最後の 1 か所**に寄せ、
 * その 1 か所が 1 セクタに収まるようにする。それより前の書き込みは、途中で
 * 落ちても名前として見えない場所 (前のエントリの rec_len の内側 = スラック、
 * まだ繋いでいないブロック) にだけ書く。
 *
 * 直す前:
 *   X1 新ブロックを既存の単一間接表へ**繋いでから**中身を書いていた。中身の
 *      書き込みが落ちると、前の持ち主のバイト列がディレクトリとして見える。
 *   X2 既存ブロックへ足すとき、前のエントリの rec_len を縮める書き込みと
 *      新しいエントリの書き込みが**別セクタ**に載り得た。後半だけ落ちると
 *      rec_len は縮んだまま、新エントリの位置にはスラックの古いバイト列
 *      (以前消したエントリ) が残り、**消した名前が復活**する。
 * ------------------------------------------------------------------------ */
int ext2_add_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name, u32 ino, u8 file_type)
{
    Ext2Inode dir_inode;
    int ret, name_len;
    u32 bi, pos, phys;
    u16 new_rec_len;
    u32 now;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    name_len = ext2_str_len(name);
    new_rec_len = (u16)((8 + name_len + 3) & ~3);

    ret = ext2_read_inode(ctx, dir_ino, &dir_inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        /* 読めなかったまま抜けると下の「新ブロック割り当て」へ落ちて、
         * まだ空きのあるブロックを見落としたままディレクトリを伸ばす。 */
        ret = ext2_bmap(ctx, &dir_inode, bi, &phys);
        if (ret != 0) return EXT2_ERR_IO;
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];
            u16 de_actual;

            if (de_reclen == 0) break;

            de_actual = (de_inode != 0) ? (u16)((8 + de_namelen + 3) & ~3) : 0;

            if (de_reclen - de_actual >= new_rec_len) {
                /* 見える状態を変えるフィールド (commit_off, 幅 commit_len) と、
                 * その前に書いておく新エントリの中身 [body_start, body_end) */
                u32 npos = pos + de_actual;
                u32 body_start = npos;
                u32 body_end = npos + 8 + (u32)name_len;
                u32 commit_off, commit_len;

                if (de_inode != 0) {
                    /* 分割: 前のエントリの rec_len を縮めた瞬間に新エントリが見える */
                    commit_off = pos + 4;
                    commit_len = 2;
                } else {
                    /* 空きエントリの再利用: inode 番号を入れた瞬間に見える */
                    commit_off = npos;
                    commit_len = 4;
                    body_start = npos + 4;
                }

                /* 1. 中身 (まだ見えない場所) */
                if (de_inode != 0) {
                    *(u32 *)&ext2_g_aux[npos]     = ino;
                    *(u16 *)&ext2_g_aux[npos + 4] = de_reclen - de_actual;
                } else {
                    *(u32 *)&ext2_g_aux[npos]     = 0;   /* まだ見せない */
                }
                ext2_g_aux[npos + 6] = (u8)name_len;
                ext2_g_aux[npos + 7] = file_type;
                ext2_mem_copy(&ext2_g_aux[npos + 8], name, (u32)name_len);

                if (!ext2_same_sector(commit_off < body_start ? commit_off : body_start,
                                      body_end > commit_off + commit_len
                                          ? body_end : commit_off + commit_len)) {
                    /* 中身と見せる瞬間が別セクタに載る: 中身だけ先に書く。
                     * 落ちても見えない (スラック / inode 0 のまま) */
                    ret = ext2_write_block(ctx, phys, ext2_g_aux);
                    if (ret != 0) return EXT2_ERR_IO;
                }

                /* 2. 見せる (1 セクタに収まるフィールド 1 つ) */
                if (de_inode != 0) {
                    *(u16 *)&ext2_g_aux[pos + 4] = de_actual;
                } else {
                    *(u32 *)&ext2_g_aux[npos] = ino;
                }
                ret = ext2_write_block(ctx, phys, ext2_g_aux);
                if (ret != 0) return EXT2_ERR_IO;

                now = ext2_current_time();
                dir_inode.mtime = now;
                ext2_write_inode(ctx, dir_ino, &dir_inode);
                ext2_ns_touch(ctx);
                return EXT2_OK;
            }
            pos += de_reclen;
        }
    }

    /* 新ブロック割り当て (票 B8 往復 4 で順序を変えた)
     *   1. 新ブロックの中身を書く     … まだどこからも辿れない。落ちたら返す
     *   2. ディレクトリの size を伸ばして inode を書く
     *                                  … まだ繋いでいないので、落ちたら返す。
     *                                    届いていれば size だけ先に伸びる (末尾の
     *                                    穴)。**穴は次の追加で埋まるとは限らない** —
     *                                    次の名前が既存ブロックのスラックに収まれば
     *                                    穴は残る (往復 5 レビュー)。穴は i_size が
     *                                    大きいだけで参照を持たないので、e2fsck の
     *                                    i_size 修正で済む (漏れ側)
     *   3. 繋ぐ (ext2_bmap_set)        … 既存の間接表ならここで媒体に載る
     *   4. inode を書く                … 直接ポインタ / 新しい単一間接表への
     *                                    ポインタはここで媒体に載る
     * どの段で落ちても、繋がったブロックは中身が書けていて、size の内側にある。 */
    {
        int new_blk = ext2_alloc_block(ctx);
        u32 new_size;
        if (new_blk < 0) return EXT2_ERR_NOSPC;

        /* alloc_block が g_aux を使った後で組む。bmap_set も g_aux を潰すので
         * 中身はその前に書き終える (gotcha §4-24) */
        ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
        *(u32 *)&ext2_g_aux[0]     = ino;
        *(u16 *)&ext2_g_aux[4]     = (u16)EXT2_BLOCK_SIZE;
        ext2_g_aux[6] = (u8)name_len;
        ext2_g_aux[7] = file_type;
        ext2_mem_copy(&ext2_g_aux[8], name, (u32)name_len);

        ret = ext2_write_block(ctx, (u32)new_blk, ext2_g_aux);
        if (ret != 0) {
            if (ext2_free_block(ctx, (u32)new_blk) != 0) { /* 漏れ */ }
            return EXT2_ERR_IO;
        }

        now = ext2_current_time();
        new_size = (bi + 1) * EXT2_BLOCK_SIZE;
        if (dir_inode.size < new_size) dir_inode.size = new_size;
        dir_inode.mtime = now;
        ret = ext2_write_inode(ctx, dir_ino, &dir_inode);
        if (ret != 0) {
            if (ext2_free_block(ctx, (u32)new_blk) != 0) { /* 漏れ */ }
            return EXT2_ERR_IO;
        }

        ret = ext2_bmap_set(ctx, &dir_inode, bi, (u32)new_blk);
        if (ret != 0) {
            /* ディレクトリの表は媒体上の inode から辿れる。IO なら new_blk が
             * 表に載ったかもしれないので**返さない** (漏れで止める)。
             * NOSPC ならどこにも載っていない (票 B8 往復 3)。 */
            if (ret == EXT2_ERR_NOSPC &&
                ext2_free_block(ctx, (u32)new_blk) != 0) { /* 漏れ */ }
            return ret;
        }

        dir_inode.blocks += 2;
        /* ここは**新しいブロックへの参照そのもの**を運ぶことがある (往復 3、
         * 段 C の掃引で発見)。書けなければ名前は辿れないかもしれないので
         * 成功と言わない。書けたか区別できないので、ブロックは返さない。 */
        ret = ext2_write_inode(ctx, dir_ino, &dir_inode);
        if (ret != 0) return EXT2_ERR_IO;
    }
    ext2_ns_touch(ctx);
    return EXT2_OK;
}

int ext2_delete_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name)
{
    Ext2Inode dir_inode;
    int ret, name_len;
    u32 bi, pos, prev_pos, phys;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    name_len = ext2_str_len(name);

    ret = ext2_read_inode(ctx, dir_ino, &dir_inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        ret = ext2_bmap(ctx, &dir_inode, bi, &phys);
        if (ret != 0) return EXT2_ERR_IO;
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0; prev_pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];

            if (de_reclen == 0) break;

            if (de_inode != 0 && de_namelen == (u8)name_len) {
                if (ext2_str_ncmp(name, (const char *)&ext2_g_aux[pos + 8], name_len) == 0) {
                    if (pos != prev_pos) {
                        u16 prev_reclen = *(u16 *)&ext2_g_aux[prev_pos + 4];
                        *(u16 *)&ext2_g_aux[prev_pos + 4] = prev_reclen + de_reclen;
                    }
                    /* 前のエントリへ併合する場合も**消したエントリの inode 番号を
                     * 0 にする** (票 B8 往復 4 / X2)。以前は rec_len を伸ばすだけで
                     * バイト列をスラックに残したので、後の追加が部分書き込みで
                     * 落ちると、そのバイト列が「生きた名前」として復活した
                     * (その inode 番号が別ファイルに再利用されていれば、そのファイル
                     * を別名で指し、unlink でファイルを壊す)。
                     * rec_len と inode 番号が別セクタに載っても、どちらか一方が
                     * 届けばエントリは見えなくなる。 */
                    *(u32 *)&ext2_g_aux[pos] = 0;
                    ret = ext2_write_block(ctx, phys, ext2_g_aux);
                    if (ret != 0) return EXT2_ERR_IO;
                    dir_inode.mtime = ext2_current_time();
                    ext2_write_inode(ctx, dir_ino, &dir_inode);
                    ext2_ns_touch(ctx);
                    return EXT2_OK;
                }
            }
            prev_pos = pos;
            pos += de_reclen;
        }
    }
    return EXT2_ERR_NOTFOUND;
}

/* ======================================================================== */
/*  mkdir / rmdir                                                            */
/* ======================================================================== */

int ext2_mkdir(Ext2Ctx *ctx, u32 parent_ino, const char *name)
{
    int new_ino, new_blk;
    Ext2Inode inode, parent_inode;
    u32 now, pos;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    ret = ext2_check_writable(ctx);   /* エラー状態なら断る (票 B8 往復 5) */
    if (ret != 0) return ret;

    /* 存在確認は **3 値で受ける** (票 B8 / Codex 実装レビュー P1-1)。
     * 「EXT2_OK のときだけ拒否」だと I/O エラーでも下の割当・作成へ進み、
     * 既にある名前と同名のディレクトリを二重に作ってしまう (実測: 18 セクタ
     * 書き込み、同名エントリ 2 件)。**何も書かずに中断する**のが正解。 */
    {
        u32 tmp;
        ret = ext2_find_entry(ctx, parent_ino, name, &tmp, (u8 *)0);
        if (ret == EXT2_OK) return EXT2_ERR_EXIST;
        if (ret != EXT2_ERR_NOTFOUND) return ret;
    }

    /* 親の links_count は u16。上限を越えて回り込むと「名前の数 > links」に
     * なるので、何も書く前に断る (票 B8 往復 5、レビュー非 blocker) */
    ret = ext2_read_inode(ctx, parent_ino, &parent_inode);
    if (ret != 0) return ret;
    if (parent_inode.links_count >= EXT2_LINK_MAX) return EXT2_ERR_MLINK;

    new_ino = ext2_alloc_inode(ctx);
    if (new_ino < 0) return EXT2_ERR_NOSPC;
    new_blk = ext2_alloc_block(ctx);
    if (new_blk < 0) {
        if (ext2_free_inode(ctx, (u32)new_ino) != 0) { /* 漏れ */ }
        return EXT2_ERR_NOSPC;
    }

    /* 親の links_count を**先に**上げる (票 B8 往復 4)。新しいディレクトリの
     * ".." が媒体に載った時点で親を指す名前が 1 つ増えるので、それより前に
     * 数を上げておけば、どこで落ちても「名前の数 <= links_count」が保てる
     * (多い側は孤児・漏れで、e2fsck が直す)。 */
    ret = ext2_read_inode(ctx, parent_ino, &parent_inode);
    if (ret == 0) {
        parent_inode.links_count++;
        ret = ext2_write_inode(ctx, parent_ino, &parent_inode);
    }
    if (ret != 0) {
        if (ext2_free_block(ctx, (u32)new_blk) != 0) { /* 漏れ */ }
        if (ext2_free_inode(ctx, (u32)new_ino) != 0) { /* 漏れ */ }
        return EXT2_ERR_IO;
    }

    now = ext2_current_time();
    ext2_mem_zero(&inode, sizeof(inode));
    inode.mode = (u16)(EXT2_S_IFDIR | 0755);
    inode.size = EXT2_BLOCK_SIZE;
    inode.atime = now; inode.ctime = now; inode.mtime = now;
    inode.links_count = 2;
    inode.blocks = 2;
    inode.block[0] = (u32)new_blk;

    /* "." と ".." */
    ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
    pos = 0;
    *(u32 *)&ext2_g_aux[pos] = (u32)new_ino;
    *(u16 *)&ext2_g_aux[pos + 4] = 12;
    ext2_g_aux[pos + 6] = 1; ext2_g_aux[pos + 7] = EXT2_FT_DIR;
    ext2_g_aux[pos + 8] = '.';
    pos = 12;
    *(u32 *)&ext2_g_aux[pos] = parent_ino;
    *(u16 *)&ext2_g_aux[pos + 4] = (u16)(EXT2_BLOCK_SIZE - 12);
    ext2_g_aux[pos + 6] = 2; ext2_g_aux[pos + 7] = EXT2_FT_DIR;
    ext2_g_aux[pos + 8] = '.'; ext2_g_aux[pos + 9] = '.';

    ret = ext2_write_block(ctx, (u32)new_blk, ext2_g_aux);
    if (ret != 0) {
        /* inode はまだ書いていないので、new_blk を指すものは媒体に無い。
         * 親の links は上げたまま (多い側 = 安全側) */
        if (ext2_free_block(ctx, (u32)new_blk) != 0) { /* 漏れ */ }
        if (ext2_free_inode(ctx, (u32)new_ino) != 0) { /* 漏れ */ }
        return EXT2_ERR_IO;
    }

    /* 以前は失敗を捨てて名前を付けていた — 名前が**書けていない inode**
     * (前の持ち主の内容) を指すことになる。書けたか区別できないので
     * **何も返さない** (書けていれば new_blk を指している。票 B8 往復 3)。 */
    ret = ext2_write_inode(ctx, (u32)new_ino, &inode);
    if (ret != 0) return EXT2_ERR_IO;

    ret = ext2_add_entry(ctx, parent_ino, name, (u32)new_ino, EXT2_FT_DIR);
    if (ret != 0) {
        /* ext2_create と揃える (票 B8 往復 4、レビュー非 blocker):
         * NOSPC なら名前は載っていないので、参照を外してから inode を返す。
         * それ以外は名前が載ったか区別できないので何も触らない。 */
        if (ret == EXT2_ERR_NOSPC) {
            int leaked = 0;
            inode.links_count = 0;
            inode.dtime = ext2_current_time();
            if (ext2_truncate_blocks(ctx, (u32)new_ino, &inode, &leaked) == EXT2_OK) {
                if (ext2_free_inode(ctx, (u32)new_ino) != 0) { /* 漏れ */ }
                /* ".." はもう辿れないので、親の links を戻してよい */
                if (ext2_read_inode(ctx, parent_ino, &parent_inode) == 0 &&
                    parent_inode.links_count > 0) {
                    parent_inode.links_count--;
                    if (ext2_write_inode(ctx, parent_ino, &parent_inode) != 0) {
                        /* 多いまま (安全側) */
                    }
                }
            }
        }
        return ret;
    }

    {
        u32 dir_group = ((u32)new_ino - 1) / ctx->sb_info.inodes_per_group;
        if (dir_group < ctx->num_groups) {
            ctx->gd_table[dir_group].used_dirs++;
            ext2_meta_touch(ctx);
        }
    }
    /* write-through の約束 (戻った時点でディスクが正しい) を守れたかを返す */
    return ext2_sync(ctx);
}

/* 1 = 空、0 = 空ではない、負値 = **判定できなかった** (票 B8)。
 * 以前は読めなかったときも 0 を返していたので、rmdir が I/O エラーを
 * NOTEMPTY と名乗っていた (拒否自体は安全側だが、理由が偽になる)。 */
static int ext2_is_dir_empty(Ext2Ctx *ctx, u32 dir_ino)
{
    Ext2Inode inode;
    int ret;
    u32 bi, pos, phys;

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        ret = ext2_bmap(ctx, &inode, bi, &phys);
        if (ret != 0) return EXT2_ERR_IO;
        if (phys == 0) break;
        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];
            if (de_reclen == 0) break;
            if (de_inode != 0) {
                if (!(de_namelen == 1 && ext2_g_aux[pos + 8] == '.') &&
                    !(de_namelen == 2 && ext2_g_aux[pos + 8] == '.' && ext2_g_aux[pos + 9] == '.')) {
                    return 0;
                }
            }
            pos += de_reclen;
        }
    }
    return 1;
}

int ext2_rmdir(Ext2Ctx *ctx, u32 parent_ino, const char *name)
{
    u32 ino;
    u8 ftype;
    Ext2Inode inode, parent_inode;
    int ret;
    int free_ret = EXT2_OK;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    ret = ext2_check_writable(ctx);   /* エラー状態なら断る (票 B8 往復 5) */
    if (ret != 0) return ret;

    ret = ext2_find_entry(ctx, parent_ino, name, &ino, &ftype);
    if (ret != 0) return ret;
    if (ftype != EXT2_FT_DIR) return EXT2_ERR_NOTDIR;
    ret = ext2_is_dir_empty(ctx, ino);
    if (ret < 0) return ret;          /* 判定できなかった。NOTEMPTY と偽らない */
    if (!ret) return EXT2_ERR_NOTEMPTY;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;

    ret = ext2_delete_entry(ctx, parent_ino, name);
    if (ret != 0) return ret;

    /* ext2_unlink と同じ扱い (票 B8 往復 3、理由は ext2_unlink の同じ箇所):
     * 名前は既に消えているので「消えていない」とは言えない。
     *   1. links 0・dtime 付き・ポインタ 0 の inode を**先に書く**
     *      -> 書けなければ**何も返さない** (孤児として残す)
     *   2. ブロックを返す -> 返しきれなくても漏れで済む
     *   3. inode を返す (もう何も指していない)
     *   4. 親の links_count を下げる — **1 が成功したときだけ** (票 B8 往復 4)。
     *      1 が落ちると孤児のディレクトリブロックの ".." が親を指したまま
     *      なので、先に下げると「親を指す名前の数 > links_count」になる。 */
    {
        int leaked = 0;
        inode.links_count = 0;
        inode.dtime = ext2_current_time();
        ret = ext2_truncate_blocks(ctx, ino, &inode, &leaked);
        if (ret != 0) {
            free_ret = ret;
        } else {
            free_ret = ext2_free_inode(ctx, ino);
            if (leaked) free_ret = EXT2_ERR_IO;

            ret = ext2_read_inode(ctx, parent_ino, &parent_inode);
            if (ret == 0) {
                if (parent_inode.links_count > 0) parent_inode.links_count--;
                parent_inode.mtime = ext2_current_time();
                if (ext2_write_inode(ctx, parent_ino, &parent_inode) != 0) {
                    /* 多いまま (安全側) */
                }
            }
        }
    }

    {
        u32 dir_group = (ino - 1) / ctx->sb_info.inodes_per_group;
        if (dir_group < ctx->num_groups) {
            ctx->gd_table[dir_group].used_dirs--;
            ext2_meta_touch(ctx);
        }
    }
    ret = ext2_sync(ctx);
    if (ret != 0) return ret;
    return free_ret;
}

/* ======================================================================== */
/*  rename                                                                  */
/* ======================================================================== */

/* ディレクトリ dir_ino の ".." が指す inode を *out へ。
 * 戻り値 EXT2_OK = 引けた / 負値 = 引けなかった (票 B8)。
 * 以前は u32 の 0 で「無い」と「読めなかった」を兼ねていたので、
 * 呼び手の循環検査が I/O エラーを「祖先ではない」と読んでいた。 */
static int ext2_parent_of(Ext2Ctx *ctx, u32 dir_ino, u32 *out)
{
    Ext2Inode inode;
    u32 phys, pos;
    int ret;

    *out = 0;
    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;
    ret = ext2_bmap(ctx, &inode, 0, &phys);
    if (ret != 0) return EXT2_ERR_IO;
    if (phys == 0) return EXT2_ERR_IO;   /* ディレクトリに先頭ブロックが無い */
    if (ext2_read_block(ctx, phys, ext2_g_aux) != 0) return EXT2_ERR_IO;

    pos = 0;
    while (pos < EXT2_BLOCK_SIZE) {
        u32 de_inode   = *(u32 *)&ext2_g_aux[pos];
        u16 de_reclen  = *(u16 *)&ext2_g_aux[pos + 4];
        u8  de_namelen = ext2_g_aux[pos + 6];
        if (de_reclen == 0) break;
        if (de_inode != 0 && de_namelen == 2 &&
            ext2_g_aux[pos + 8] == '.' && ext2_g_aux[pos + 9] == '.') {
            *out = de_inode;
            return EXT2_OK;
        }
        pos += de_reclen;
    }
    /* ".." が無いディレクトリは壊れている。NOTFOUND は呼び手 (rename) から
     * 見ると「元の名前が無い」と読めてしまうので使わない。 */
    return EXT2_ERR_IO;
}

/* ディレクトリ dir_ino の ".." を new_parent に書き換える */
static int ext2_set_dotdot(Ext2Ctx *ctx, u32 dir_ino, u32 new_parent)
{
    Ext2Inode inode;
    u32 phys, pos;
    int ret;

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;
    ret = ext2_bmap(ctx, &inode, 0, &phys);
    if (ret != 0) return EXT2_ERR_IO;
    if (phys == 0) return EXT2_ERR_IO;
    ret = ext2_read_block(ctx, phys, ext2_g_aux);
    if (ret != 0) return EXT2_ERR_IO;

    pos = 0;
    while (pos < EXT2_BLOCK_SIZE) {
        u32 de_inode   = *(u32 *)&ext2_g_aux[pos];
        u16 de_reclen  = *(u16 *)&ext2_g_aux[pos + 4];
        u8  de_namelen = ext2_g_aux[pos + 6];
        if (de_reclen == 0) break;
        if (de_inode != 0 && de_namelen == 2 &&
            ext2_g_aux[pos + 8] == '.' && ext2_g_aux[pos + 9] == '.') {
            *(u32 *)&ext2_g_aux[pos] = new_parent;
            ret = ext2_write_block(ctx, phys, ext2_g_aux);
            return (ret != 0) ? EXT2_ERR_IO : EXT2_OK;
        }
        pos += de_reclen;
    }
    return EXT2_ERR_NOTFOUND;
}

/* new_dir が ino 自身、または ino の子孫か (ディレクトリを自分の中へ移す
 * 循環を防ぐ)。".." を root まで辿る。
 *
 * 1 = 自身か子孫、0 = 違う、負値 = **判定できなかった** (票 B8)。
 * 読めなかったのを 0 (「違う」) と言うと、ディレクトリを自分の配下へ
 * 移す rename が通って木が輪になる。 */
static int ext2_is_self_or_descendant(Ext2Ctx *ctx, u32 ino, u32 new_dir)
{
    u32 cur = new_dir;
    int hops = 0;
    while (cur != 0 && hops < EXT2_RENAME_MAX_DEPTH) {
        if (cur == ino) return 1;
        if (cur == EXT2_ROOT_INO) return 0;
        {
            u32 parent = 0;
            int ret = ext2_parent_of(ctx, cur, &parent);
            if (ret != 0) return ret;
            if (parent == 0 || parent == cur) return 0;
            cur = parent;
        }
        hops++;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 *  ディレクトリの rename — **旧名を先に消す** (票 B8 往復 5 / ユーザー決裁 1)
 *
 *  往復 4 まではファイルと同じ「新名を載せてから旧名を消す」だったので、
 *  旧名の削除 (6 セクタの I/O) のどれか 1 回の失敗で**1 つのディレクトリに
 *  名前が 2 つ**残った。links は合っていても OS32 の rmdir は links を見ずに
 *  inode を返すので、残った名前が解放済み inode を指し (dangling)、2 名からは
 *  ".." しか辿らない循環検査をすり抜けてディレクトリの輪も作れた。
 *
 *  ディレクトリは「先に旧名を消し、最後に新名を載せる」。途中で落ちると
 *  **名前 0 個の孤児**になる。孤児は OS32 のどの操作からも辿れないので、後続の
 *  rmdir / mkdir / rename に化けない (ホストの e2fsck が lost+found へ回収する)。
 *  往復 3 の「破壊より漏れ」と同じ方針。
 *
 *  守る不変条件:
 *    (a) どの inode も、それを指す名前 ("." ".." を含む) の数 <= links_count
 *    (b) ディレクトリ inode を指す "." ".." 以外の名前は 1 つ以下
 *    (c) ".." を新しい親へ向ける前に新しい親の links を上げ、
 *        旧親の links は ".." が離れた後で下げる
 *    (孤児の ".." が旧親を指したまま残るのは許容 — e2fsck が直す)
 *
 *  段と、その段で落ちたときの媒体 (別の親へ移すとき。同じ親なら 2〜4 が無い):
 *    0. 新しい親の links が上限でないか読む        … 何も書いていない
 *    1. 旧名を消す (ext2_delete_entry)              … 名前 1 (届かなかった) か
 *                                                     0 (孤児)。links・".." は元
 *    2. 新しい親の links +1                          … 孤児。新親の links は元か +1
 *    3. D の ".." を新しい親へ (1 セクタの 4B = 二値) … 孤児。".." は旧親か新親、
 *                                                     どちらも数えられている
 *    4. 旧親の links -1                              … 孤児。旧親の links は元か -1
 *    5. 新名を載せる (ext2_add_entry)                … 名前 0 (孤児) か 1 (完了)。
 *                                                     NOSPC なら巻き戻す (下)
 *    6. D の ctime                                  … 完了済み。失敗はエラー状態
 *                                                     を立てるが OK を返す
 *  D 自身の links_count は動かさない (名前は 1 -> 0 -> 1 で、多い側にしか振れない)。
 *
 *  NOSPC の巻き戻し (新名はどこにも載っていない): 旧親の links +1 -> ".." を
 *  旧親へ -> 新親の links -1 -> 旧名を載せ直す。どこで落ちても孤児で止まり、
 *  (a)(b)(c) は保たれる。旧名を消した隙間は同じ長さの名前がちょうど入るので、
 *  載せ直しは NOSPC にならない。
 * ------------------------------------------------------------------------ */
static int ext2_rename_dir(Ext2Ctx *ctx, u32 ino, u32 old_dir, const char *old_name,
                           u32 new_dir, const char *new_name)
{
    Ext2Inode pinode, dinode;
    int ret;
    int cross = (old_dir != new_dir);

    /* 0 */
    if (cross) {
        ret = ext2_read_inode(ctx, new_dir, &pinode);
        if (ret != 0) return ret;
        if (pinode.links_count >= EXT2_LINK_MAX) return EXT2_ERR_MLINK;
    }

    /* 1 */
    ret = ext2_delete_entry(ctx, old_dir, old_name);
    if (ret != 0) return ret;

    if (cross) {
        /* 2 */
        ret = ext2_read_inode(ctx, new_dir, &pinode);
        if (ret == 0) {
            pinode.links_count++;
            ret = ext2_write_inode(ctx, new_dir, &pinode);
        }
        if (ret != 0) return EXT2_ERR_IO;

        /* 3 */
        ret = ext2_set_dotdot(ctx, ino, new_dir);
        if (ret != 0) return ret;

        /* 4 */
        ret = ext2_read_inode(ctx, old_dir, &pinode);
        if (ret == 0) {
            if (pinode.links_count > 0) pinode.links_count--;
            ret = ext2_write_inode(ctx, old_dir, &pinode);
        }
        if (ret != 0) return EXT2_ERR_IO;
    }

    /* 5 */
    ret = ext2_add_entry(ctx, new_dir, new_name, ino, EXT2_FT_DIR);
    if (ret == EXT2_ERR_NOSPC) {
        /* 新名は載っていない。旧名へ戻す (落ちたら孤児で止まる) */
        if (cross) {
            if (ext2_read_inode(ctx, old_dir, &pinode) != 0) return ret;
            pinode.links_count++;
            if (ext2_write_inode(ctx, old_dir, &pinode) != 0) return ret;
            if (ext2_set_dotdot(ctx, ino, old_dir) != 0) return ret;
            if (ext2_read_inode(ctx, new_dir, &pinode) != 0) return ret;
            if (pinode.links_count > 0) pinode.links_count--;
            if (ext2_write_inode(ctx, new_dir, &pinode) != 0) return ret;
        }
        if (ext2_add_entry(ctx, old_dir, old_name, ino, EXT2_FT_DIR) != 0) {
            /* 孤児で止まる */
        }
        return ret;
    }
    if (ret != 0) return ret;

    /* 6 — 名前は完成している。失敗は ext2_write_block がエラー状態にするが、
     * 「rename できなかった」とは言わない (言うと呼び手はやり直そうとする) */
    if (ext2_read_inode(ctx, ino, &dinode) == 0) {
        dinode.ctime = ext2_current_time();
        if (ext2_write_inode(ctx, ino, &dinode) != 0) { /* ctime だけ古い */ }
    }

    return ext2_sync(ctx);
}

int ext2_rename(Ext2Ctx *ctx, u32 old_dir, const char *old_name,
                u32 new_dir, const char *new_name)
{
    u32 ino, dst_ino;
    u8 ftype, dst_type;
    Ext2Inode inode;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    ret = ext2_check_writable(ctx);   /* エラー状態なら断る (票 B8 往復 5) */
    if (ret != 0) return ret;
    if (!old_name[0] || !new_name[0]) return EXT2_ERR_INVAL;

    ret = ext2_find_entry(ctx, old_dir, old_name, &ino, &ftype);
    if (ret != 0) return ret;

    /* 同一エントリへの rename は no-op */
    if (old_dir == new_dir && ext2_str_ncmp(old_name, new_name,
            ext2_str_len(old_name) + 1) == 0) {
        return EXT2_OK;
    }

    /* 移動先に同名がある場合: ディレクトリは上書きしない。
     * ファイル同士なら POSIX と同じく置き換える。
     *
     * **3 値で受ける** (票 B8 / Codex 実装レビュー P1-3)。読めなかったのを
     * 「無い」と読み替えると置き換えの分岐を丸ごと飛ばし、下の add_entry が
     * 宛先に同名エントリを二重に作ったうえで delete_entry が移動元の名前を
     * 消す — **名前が片方だけ消えて二重になる**(実測: 10 セクタ書き込み)。
     * 判定できないなら何も書かずに中断する。 */
    ret = ext2_find_entry(ctx, new_dir, new_name, &dst_ino, &dst_type);
    if (ret == EXT2_OK) {
        if (dst_ino == ino) {
            if (ftype == EXT2_FT_DIR) {
                /* **1 つのディレクトリに名前が 2 つ** = 媒体が壊れている
                 * (票 B8 往復 5)。往復 5 以降の rename はこの状態を作らない
                 * (旧名を先に消す) ので、ここに来るのは往復 4 以前のコードが
                 * 書いた媒体か、別の破損だけ。以前は「ハードリンク同士」として
                 * OK を返し 2 名を残していた。完了させようとはせず (".." や
                 * 親の links がどこまで進んでいたか分からない)、メタデータの
                 * 不整合としてエラー状態にして断る。 */
                ext2_fs_error(ctx);
                return EXT2_ERR_IO;
            }
            return EXT2_OK;   /* ファイルのハードリンク同士 (POSIX どおり何もしない) */
        }
        if (dst_type == EXT2_FT_DIR) return EXT2_ERR_EXIST;
        if (ftype == EXT2_FT_DIR) return EXT2_ERR_NOTDIR;
        ret = ext2_unlink(ctx, new_dir, new_name);
        if (ret != 0) return ret;
    } else if (ret != EXT2_ERR_NOTFOUND) {
        return ret;
    }

    if (ftype == EXT2_FT_DIR) {
        if (old_dir != new_dir) {
            int desc = ext2_is_self_or_descendant(ctx, ino, new_dir);
            if (desc < 0) return desc;      /* 判定できなかった。INVAL と偽らない */
            if (desc) return EXT2_ERR_INVAL;
        }
        return ext2_rename_dir(ctx, ino, old_dir, old_name, new_dir, new_name);
    }

    /* ---- ファイルの名前の付け替え (票 B8 往復 4 / X3、往復 5 もこのまま) ----
     *
     * 不変条件: **どの inode も、それを指す名前の数 <= links_count**。
     * ファイルのハードリンクは正当なので、失敗して名前が 2 つ残っても整合する。
     *
     * 順序と、各段で落ちたときの媒体:
     *   1. 移す inode の links_count を +1 して書く
     *        落ちた: 名前 1、links は元か +1 (多い側 = 安全)。何も付け替えていない
     *   3. 新しい名前を載せる (ext2_add_entry)
     *        落ちた: 名前 1 か 2、links は +1 済み。NOSPC なら名前は載って
     *        いないので 1 を戻す (戻せなくても多い側)
     *   4. 古い名前を消す (ext2_delete_entry)
     *        落ちた: 名前 2 か 1、links は +1 済み = 整合。**巻き戻さない**
     *   6. 移す inode の links_count を -1 して書く (ctime も)
     *        落ちた: links が 1 多い (孤児側。e2fsck が直す)
     * (段 2 / 5 は別の親へ移すディレクトリの段だったので、往復 5 で
     *  ext2_rename_dir へ移した。番号は往復 4 の記録と揃えて残している。)
     */
    /* 1 */
    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;
    if (inode.links_count >= EXT2_LINK_MAX) return EXT2_ERR_MLINK;
    inode.links_count++;
    inode.ctime = ext2_current_time();
    ret = ext2_write_inode(ctx, ino, &inode);
    if (ret != 0) return EXT2_ERR_IO;

    /* 3 */
    ret = ext2_add_entry(ctx, new_dir, new_name, ino, ftype);
    if (ret != 0) {
        if (ret == EXT2_ERR_NOSPC) {
            /* 名前は載っていない。上げた数を戻す (落ちても多い側) */
            if (ext2_read_inode(ctx, ino, &inode) == 0 && inode.links_count > 0) {
                inode.links_count--;
                if (ext2_write_inode(ctx, ino, &inode) != 0) { /* 多いまま */ }
            }
        }
        return ret;
    }

    /* 4 */
    ret = ext2_delete_entry(ctx, old_dir, old_name);
    if (ret != 0) return ret;

    /* 6 */
    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret == 0) {
        if (inode.links_count > 0) inode.links_count--;
        inode.ctime = ext2_current_time();
        ret = ext2_write_inode(ctx, ino, &inode);
    }
    if (ret != 0) return EXT2_ERR_IO;

    /* write-through の約束 (戻った時点でディスクが正しい) を守れたかを返す */
    return ext2_sync(ctx);
}

/* ======================================================================== */
/*  パス検索                                                                */
/* ======================================================================== */

int ext2_lookup(Ext2Ctx *ctx, const char *path, u32 *out_ino)
{
    u32 current_ino = EXT2_ROOT_INO;
    char component[EXT2_NAME_LEN + 1];
    int i, ci;
    u32 found_ino;
    u8 found_type;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    if (!path || path[0] == '\0') { *out_ino = EXT2_ROOT_INO; return EXT2_OK; }

    i = 0;
    if (path[0] == '/') i++;

    while (path[i] != '\0') {
        ci = 0;
        while (path[i] != '\0' && path[i] != '/' && ci < EXT2_NAME_LEN) {
            component[ci++] = path[i++];
        }
        component[ci] = '\0';
        if (ci == 0) { if (path[i] == '/') { i++; continue; } break; }

        /* **畳まない** (票 B8)。以前は I/O エラーもここで NOTFOUND に
         * なっていたので、「読めなかった」が「無い」として上へ伝わり、
         * open の O_CREAT 経路が既存ファイルを空で作り直していた。 */
        ret = ext2_find_entry(ctx, current_ino, component, &found_ino, &found_type);
        if (ret != 0) return ret;
        current_ino = found_ino;
        if (path[i] == '/') i++;
    }

    *out_ino = current_ino;
    return EXT2_OK;
}

/* ======================================================================== */
