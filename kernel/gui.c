/* ======================================================================== */
/*  GUI.C — GUI シェル v1.1 のカーネル背骨 (K レーン K1)                     */
/*                                                                          */
/*  gui_call / gui_register / gui_owner_exit / sys_switch_shell を実装する。 */
/*  カーネルは op の意味を知らない: 登録ハンドラへ転送し owner を付けるだけ。 */
/*  WM からアプリへのコールバック経路は作らない (契約 T1: 再入の根絶)。       */
/* ======================================================================== */

#include "gui.h"
#include "kstring.h"
#include "kbd.h"      /* kbd_set_gui_mode (K2-B) */
#include "ime.h"      /* ime_set_render (レビュー #4 ①: gshell 終了時の防御的リセット) */

/* res_owner_get() は fs/fd_redirect.c。FD と同じ「確保した実行レベル」。
 * ドライバ/カーネル各所と同じ流儀で extern 宣言する (-Ifs 非依存)。 */
extern int res_owner_get(void);

/* GUI_SHELL_OWNER (= 1) は gui.h。K2 の syscall 境界ポンプも同じ値を使う。 */

/* ======== WM 登録状態 ======== */
/* gui_register で shell 帯から一度だけ登録される。CPL=0 のシェル帯コード
 * だけが書き手なので追加のロックは不要。 */
static GuiHandler g_gui_handler = 0;
static void      *g_gui_pump    = 0;

/* 次に起動するシェルのパス (契約 T9)。空文字列 = 記録なし。 */
static char g_next_shell[OS32_MAX_PATH];

/* ======================================================================== */
/*  gui_call — アプリ → WM の唯一の入口 (契約 T1)                            */
/* ======================================================================== */
i32 gui_call(u32 op, u32 arg)
{
    if (g_gui_handler == 0) {
        return OS32_ERR_NOSYS;
    }
    /* op の意味は解釈しない。呼び出し元の owner を付けて転送するだけ。 */
    return g_gui_handler(op, arg, res_owner_get());
}

/* ======================================================================== */
/*  gui_register — WM (gshell) がハンドラを登録する                          */
/*  shell 帯 (owner 1, CPL=0) からのみ。それ以外は OS32_ERR_INVAL。          */
/* ======================================================================== */
i32 gui_register(void *handler, void *pump)
{
    if (res_owner_get() != GUI_SHELL_OWNER) {
        return OS32_ERR_INVAL;
    }
    if (handler == 0) {
        return OS32_ERR_INVAL;
    }
    g_gui_handler = (GuiHandler)handler;
    g_gui_pump    = pump;   /* K2 の syscall 境界ポンプが呼ぶ (exec/exec.c) */

    /* WM が入ったのでキーボードを GUI モードへ (K2-B、W1 申し送り ①)。
     * WM は raw リングだけを読むので、cooked リング (kbd_buf) に積み続けると
     * 32 打鍵で溢れて kbd_dropped が増え、実際には落ちていない打鍵が
     * 偽の OVERFLOW として WM → アプリへ伝わる。GUI 中は積まない。 */
    kbd_set_gui_mode(1);
    return 0;
}

/* ======================================================================== */
/*  gui_owner_exit — 所有者による回収の口 (契約 T4 / U8)                     */
/*                                                                          */
/*  exec_exit() が owner 別回収の並びで呼ぶ。ハンドラが登録済みなら           */
/*  GUI_OP_OWNER_EXIT を渡し、WM がその owner のウィンドウ・サーフェス・       */
/*  タイマ・スロットを回収する (W1 が実装)。未登録なら何もしない。           */
/* ======================================================================== */
void gui_owner_exit(int owner)
{
    if (g_gui_handler != 0) {
        g_gui_handler(GUI_OP_OWNER_EXIT, 0, owner);
    }
    /* WM 自身 (gshell = shell 帯 owner 1) が終了するなら登録を解除する。
     * これをしないと、gshell.bin が抜けて同じ 0x300000 に shell.bin が
     * 載った後も g_gui_handler / g_gui_pump が旧コードを指したままになり、
     * K2 の syscall 境界ポンプが別コードを呼んでしまう (レビュー ①)。 */
    if (owner == GUI_SHELL_OWNER) {
        g_gui_handler = 0;
        g_gui_pump    = 0;
        /* CUI に戻るのでキーボードを元に戻す (cooked リングを再開)。
         * これをしないと shell.bin が載っても打鍵が届かない (K2-B)。 */
        kbd_set_gui_mode(0);
        /* FEP の描画先を TVRAM 版へ戻す (レビュー #4 ①)。gshell は自分でも
         * NULL を渡すが、クラッシュで抜けた場合も 0x300000 の旧コードを
         * 指したままにしない。 */
        ime_set_render((void *)0);
    }
}

/* ======================================================================== */
/*  gui_get_pump — K2 用: 登録された入力ポンプを返す                         */
/* ======================================================================== */
void *gui_get_pump(void)
{
    return g_gui_pump;
}

/* ======================================================================== */
/*  sys_switch_shell — 次シェルのパスを記録する (契約 T9、K4 の入口)         */
/*                                                                          */
/*  shell 帯 (owner 1) からのみ。実際の入れ替えはカーネルのシェル起動ループ  */
/*  (K4) が gui_next_shell() を読んで行う。呼んだシェルは自分で後始末して     */
/*  exit する。                                                              */
/* ======================================================================== */
i32 sys_switch_shell(const char *path)
{
    if (res_owner_get() != GUI_SHELL_OWNER) {
        return OS32_ERR_INVAL;
    }
    if (path == 0 || path[0] == '\0') {
        return OS32_ERR_INVAL;
    }
    kstrncpy(g_next_shell, path, OS32_MAX_PATH);
    g_next_shell[OS32_MAX_PATH - 1] = '\0';
    return 0;
}

/* ======================================================================== */
/*  gui_next_shell — K4 用: 記録された次シェルのパス (無ければ NULL)         */
/* ======================================================================== */
const char *gui_next_shell(void)
{
    if (g_next_shell[0] == '\0') {
        return (const char *)0;
    }
    return g_next_shell;
}

/* ======================================================================== */
/*  gui_take_next_shell — K4 用: 要求を取り出して消す (consume 方式)         */
/*                                                                          */
/*  起動ループはこちらを使う。パス比較で「前回と違うときだけ新要求」と        */
/*  みなす方式だと、gshell のロード失敗や連続 fault で CUI に落ちた後に       */
/*  同じ /bin/gshell.bin を要求しても二度と採用されない (レビュー #3 ④)。    */
/*  戻り値: 要求があれば 1 (out にコピー)、無ければ 0。                       */
/* ======================================================================== */
int gui_take_next_shell(char *out, int cap)
{
    if (g_next_shell[0] == '\0' || out == 0 || cap <= 0) {
        return 0;
    }
    kstrncpy(out, g_next_shell, cap);
    out[cap - 1] = '\0';
    g_next_shell[0] = '\0';
    return 1;
}

/* ======================================================================== */
/*  gfx_stats / gfx_lease_palette — H1 の依頼分の weak 既定                  */
/*                                                                          */
/*  本実装は H レーンが gfx/ (gfx_core.c) に置く (票 §3「H1 からの依頼」)。  */
/*  K1 は KAPI スロットを切るだけだが、H1 未着手でもカーネルがリンクできる   */
/*  よう weak な既定を置く。H1 の強シンボルがリンク時に上書きする。          */
/*  TODO(H1): gfx/ で本実装に置き換える。                                    */
/* ======================================================================== */
__attribute__((weak)) int gfx_stats(void *out)
{
    if (out != 0) {
        kmemset(out, 0, sizeof(GFX_Stats));
    }
    return OS32_ERR_NOSYS;
}

__attribute__((weak)) int gfx_lease_palette(int first, int count, const u8 *rgb)
{
    (void)first;
    (void)count;
    (void)rgb;
    return OS32_ERR_NOSYS;
}
