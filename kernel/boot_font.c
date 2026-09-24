/* ======================================================================== */
/*  BOOT_FONT.C — 起動時の既定フォント (KCG) の読み込み                      */
/*                                                                          */
/*  正規名 SYS_FONT_DEFAULT (/sys/font/default.kcgfont) を読む。読めず、     */
/*  **かつルートの FS が FAT (FD 起動) のときだけ** 8.3 の短い名前           */
/*  SYS_FONT_DEFAULT_83 を読む — FD は FatFs (LFN なし) なので正規名を置けない */
/*  (build/packages.yaml の fd.rename)。HDD (ext2) で正規名が欠けたり壊れたり */
/*  したとき、残っている短い名前を黙って掴まないように FS を見る (Codex 実装 */
/*  レビュー)。判定は vfs_fstype("/") — /sys に別のマウントは置かないので、    */
/*  フォントはルートのマウントにある。                                       */
/*  ホスト試験は tools/tests/test_packages.py の case 8 (本ファイルを実物の  */
/*  まま取り込み、kcg_load_font / vfs_fstype を偽物にする)。                 */
/* ======================================================================== */

#include "boot_font.h"
#include "config.h"
#include "kcg.h"
#include "vfs.h"
#include "kstring.h"

int boot_font_load(void)
{
    int fret;

    fret = kcg_load_font(SYS_FONT_DEFAULT);
    if (fret != 0 && kstrcmp(vfs_fstype("/"), SYS_FONT_83_FSTYPE) == 0) {
        fret = kcg_load_font(SYS_FONT_DEFAULT_83);
    }
    return fret;
}
