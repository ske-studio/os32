/* ======================================================================== */
/*  KBD.C — PC-98 キーボードドライバ                                       */
/*                                                                          */
/*  μPD8251A経由でスキャンコードを取得し、ASCII変換してリングバッファに格納 */
/*  IRQ1割り込みで駆動                                                      */
/*                                                                          */
/*  出典: PC9800Bible §2-5 表2-14                                           */
/*  参照: FreeBSD sys/pc98/cbus/pckbd.c                                     */
/*    - IO_KBD = 0x041 (ベースアドレス)                                     */
/*    - KBD_DATA_PORT = base + 0 = 0x41                                     */
/*    - KBD_STATUS_PORT = base + 2 = 0x43 (PC-98はI/O 2バイト間隔)         */
/*    - KBDS_BUFFER_FULL = 0x0002 (8251 RxRDY)                              */
/*    - FreeBSD init_keyboard() は空 → BIOS初期化済みを前提                */
/* ======================================================================== */

#include "kbd.h"
#include "io.h"
#include "serial.h"
#include "hotdeploy.h"

extern volatile int exec_nest_level;  /* exec/exec.c */

/* 外部: irq_enable (idt.c で定義) */
extern void irq_enable(unsigned int irq);

/* 外部: V86 セッション (kernel/v86.c, kernel/v86_kbd.c)。
 * drivers/ は -Ikernel を持たないので、この 3 本だけここで宣言する
 * (irq_enable と同じ扱い)。ドライバが V86 の中身を知る必要はない。 */
extern int  v86_is_active(void);
extern void v86_request_exit(void);
extern int  v86_kbd_push(u8 scancode);

/* rshellモード判定用 (shell.cで定義) */
extern int rshell_active;

/* 外部: CPL=3 アプリの強制脱出要求 (exec/exec.c、K2 / 契約 T6)。
 * CTRL+STOP を見つけたら要求を立てるだけで、実際に畳むのは IRQ1 スタブ
 * (割り込まれた文脈が CPL=3 のとき) か次の syscall 入口。drivers/ は
 * -Iexec を持たないので irq_enable と同じ流儀で extern 宣言する。 */
extern void ring3_abort_request(void);

/* ======== シフトキー状態 ========
 * **書き込むのは kbd_irq_handler (IRQ1 ISR) だけ**。ISR は割り込みゲート
 * 経由で IF=0 のまま走り自身に再入しないので、ここでの |= / &= / ^= は
 * ロック無しで安全。カーネル側は読むだけ (ime.c 等)。
 * この所有権を破って通常コンテキストから書くなら irq_save が要る。 */
volatile u8 kbd_shift_state = 0;

/* ======== キー押下状態ビットマップ (128キー分) ======== */
/* ビット1 = 押下中, ビット0 = 離されている */
static volatile u8 kbd_key_pressed[16]; /* 128bit = 16bytes */

/* ======== リングバッファ (u16: 上位=スキャンコード, 下位=ASCII) ======== */
static volatile u16 kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;
static volatile int kbd_count = 0;

/* 待ち行列が満杯で捨てた打鍵の累計 (契約 T3、GUI v1.1)。IRQ1 ISR だけが
 * 加算する (kbd_shift_state と同じ所有権)。KAPI kbd_dropped_count() で読む。 */
static volatile u32 kbd_dropped = 0;

/* 生 make/break イベントリング (レビュー ⑥、GUI の Key down/up 用)。
 * cooked な kbd_buf (make のみ、ASCII 化) と別に、全キーの押下/離しを
 * 順序どおり積む。WM (gshell) が kbd_trygetrawkey() で読み、down=1/0 の
 * Key イベントを作る。エントリ = keycode | (down << 8)。満杯なら捨てる
 * (WM が追いつく前提。取りこぼしは kbd_dropped と別勘定にしない)。 */
static volatile u16 kbd_raw_buf[KBD_BUF_SIZE];
static volatile int kbd_raw_head = 0;
static volatile int kbd_raw_tail = 0;
static volatile int kbd_raw_count = 0;
/* raw リング満杯で捨てた生イベント数 (レビュー ①)。GUI の取りこぼし検出用に
 * kbd_dropped_count() へ合算する。break を落とすと WM の修飾状態がずれるため必須。 */
static volatile u32 kbd_raw_dropped = 0;

/* GUI モード (W1 申し送り ①、K2-B)。WM (gshell) が gui_register した間だけ 1。
 *
 * WM は raw リングだけを読む。cooked リング (kbd_buf) は誰も読まないので、
 * 積み続けると 32 打鍵で満杯になり kbd_dropped が増え続ける。それを
 * kbd_dropped_count() 経由で WM が拾うと、実際には 1 打鍵も落ちていないのに
 * ヘッダの dropped が増えて OVERFLOW が立つ (偽の取りこぼし)。
 * したがって GUI 中は cooked に積まない — 打鍵の勘定を raw 側 1 本にする。
 * kbd_dropped_count() の意味 (契約 T3: WM が差分を dropped に足す) は
 * 変わらない。ASCII 化は WM が自前の写し (gshell の input::translate) で行う。
 * rshell のシリアル入力は kbd_count を経由しないので影響を受けない。 */
static volatile int kbd_gui_mode = 0;

/* ======================================================================== */
/*  スキャンコード → ASCII 変換テーブル                                    */
/*  PC9800Bible §2-5 表2-14 の「通常」列から抽出                           */
/*  インデックス = キーコード (0x00〜0x6B)                                  */
/* ======================================================================== */

/* 通常状態 (Shift なし) */
static const u8 scancode_to_ascii[128] = {
    /* 0x00-0x0F */
    0x1B, '1', '2', '3', '4', '5', '6', '7',   /* ESC, 1-7 */
    '8',  '9', '0', '-', '^', '\\', 0x08, 0x09, /* 8-0,-,^,\,BS,TAB */
    /* 0x10-0x1F */
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',   /* Q-I */
    'o',  'p', '@', '[', 0x0D, 'a', 's', 'd',   /* O-P,@,[,ENTER,A-D */
    /* 0x20-0x2F */
    'f',  'g', 'h', 'j', 'k', 'l', ';', ':',   /* F-L,;,: */
    ']',  'z', 'x', 'c', 'v', 'b', 'n', 'm',   /* ],Z-M */
    /* 0x30-0x3F */
    ',',  '.', '/', 0,   ' ', 0,   0x12,0x03,   /* ,./ _,SPACE,XFER,RLUP(12),RLDN(03) */
    0x16, 0x7F, 0x1E, 0x1D, 0x1C, 0x1F, 0x01, 0x05,  /* INS(16),DEL,↑,←,→,↓,HOME(01),HELP(05) */
    /* 0x40-0x4F (テンキー) */
    '-',  '/', '7', '8', '9', '*', '4', '5',
    '6',  '+', '1', '2', '3', '=', '0', ',',
    /* 0x50-0x5F */
    '.',  0,   0,   0,   0,   0,   0,   0,      /* NFER,vf1-vf5 */
    0,    0,   0,   0,   0,   0,   0,   0,
    /* 0x60-0x6F (STOP,COPY,F1-F10) */
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    /* 0x70-0x7F (SHIFT,CAPS,KANA,GRPH,CTRL) */
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
};

/* Shift状態 */
static const u8 scancode_to_ascii_shift[128] = {
    /* 0x00-0x0F */
    0x1B, '!', '"', '#', '$', '%', '&', '\'',
    '(',  ')', 0,   '=', '`', '|', 0x08, 0x09,
    /* 0x10-0x1F */
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O',  'P', '~', '{', 0x0D, 'A', 'S', 'D',
    /* 0x20-0x2F */
    'F',  'G', 'H', 'J', 'K', 'L', '+', '*',
    '}',  'Z', 'X', 'C', 'V', 'B', 'N', 'M',
    /* 0x30-0x3F */
    '<',  '>', '?', '_', ' ', 0,   0x12,0x03,
    0x16, 0x7F, 0x1E, 0x1D, 0x1C, 0x1F, 0x01, 0x05,
    /* 0x40-0x7F: テンキー以降はShiftでも同じ */
    '-',  '/', '7', '8', '9', '*', '4', '5',
    '6',  '+', '1', '2', '3', '=', '0', ',',
    '.',  0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
};

/* ======================================================================== */
/*  kbd_irq_handler — IRQ1 割り込みハンドラ (Cレベル)                      */
/*  ASMスタブから呼ばれる                                                    */
/* ======================================================================== */
void kbd_irq_handler(void)
{
    u8 scancode;
    u8 keycode;
    u8 ascii;
    int is_break;

    /* μPD8251Aからスキャンコード読み取り */
    scancode = (u8)inp(KBD_DATA);

    /* V86 セッション中はキーをまるごとゲストへ回す。
     * 8251A のデータレジスタは読んだら消えるので、ここで OS32 側の
     * リングバッファにも入れると「シェルに打った覚えのない文字が
     * 溜まる」ことになる。所有権はどちらか一方しか持てない。
     * 戻り値が非 0 なら脱出ホットキー (CTRL+GRPH+DEL)。 */
    if (v86_is_active()) {
        if (v86_kbd_push(scancode)) {
            v86_request_exit();
        }
        return;
    }

    is_break = scancode & SCANCODE_BREAK;
    keycode  = scancode & SCANCODE_KEY;

    /* キー押下状態ビットマップの更新 (全キー対象) */
    if (is_break) {
        kbd_key_pressed[keycode >> 3] &= ~(1 << (keycode & 7));
    } else {
        kbd_key_pressed[keycode >> 3] |=  (1 << (keycode & 7));
    }

    /* 生イベントを raw リングへ (make も break も、全キー。IRQ 内なので保護不要) */
    if (kbd_raw_count < KBD_BUF_SIZE) {
        kbd_raw_buf[kbd_raw_tail] = (u16)keycode | (is_break ? 0 : 0x100);
        kbd_raw_tail = (kbd_raw_tail + 1) % KBD_BUF_SIZE;
        kbd_raw_count++;
    } else {
        kbd_raw_dropped++;   /* GUI が resync できるよう必ず数える (レビュー ①) */
    }

    /* シフトキー状態の更新 */
    if (keycode == KEY_SHIFT) {
        if (is_break) kbd_shift_state &= ~SHIFT_SHIFT;
        else          kbd_shift_state |=  SHIFT_SHIFT;
        return;
    }
    if (keycode == KEY_CTRL) {
        if (is_break) kbd_shift_state &= ~SHIFT_CTRL;
        else          kbd_shift_state |=  SHIFT_CTRL;
        return;
    }
    if (keycode == KEY_CAPS) {
        if (!is_break) kbd_shift_state ^= SHIFT_CAPS;
        return;
    }
    if (keycode == KEY_KANA) {
        if (!is_break) kbd_shift_state ^= SHIFT_KANA;
        return;
    }
    if (keycode == KEY_GRPH) {
        if (is_break) kbd_shift_state &= ~SHIFT_GRPH;
        else          kbd_shift_state |=  SHIFT_GRPH;
        return;
    }

    /* 強制脱出キー CTRL+STOP (契約 T6 / K2 作業 4)。
     * PC-98 で「止める」といえばこれ (V86 セッションの脱出と同じキー)。
     * CPL=3 アプリが KAPI を呼ばない計算ループに入ってしまうと syscall 境界
     * ポンプも効かないので、ここが唯一の逃げ道になる。ISR では要求を立てる
     * だけで、実際に畳むのは EOI 済みの IRQ1 スタブ (割り込まれた文脈が
     * CPL=3 のとき) か次の syscall 入口 (exec/exec.c)。
     * 打鍵としては配らない (kill した後のシェルに STOP が残らないように)。 */
    if (!is_break && keycode == KEY_STOP && (kbd_shift_state & SHIFT_CTRL)) {
        ring3_abort_request();
        return;
    }

    /* ブレイク(キー離し)はリングバッファには入れない */
    if (is_break) return;

    /* GUI モード中は cooked リングに積まない (kbd_gui_mode の説明を参照)。
     * 生イベントは上で raw リングへ積み済みなので WM は取りこぼさない。 */
    if (kbd_gui_mode) return;

    /* スキャンコード → ASCII変換 */
    if (kbd_shift_state & SHIFT_SHIFT) {
        ascii = scancode_to_ascii_shift[keycode];
    } else {
        ascii = scancode_to_ascii[keycode];
    }

    /* CAPS時の大文字小文字切替 */
    if (kbd_shift_state & SHIFT_CAPS) {
        if (ascii >= 'a' && ascii <= 'z') ascii -= 32;
        else if (ascii >= 'A' && ascii <= 'Z') ascii += 32;
    }

    /* CTRL+文字 → コントロールコード */
    if ((kbd_shift_state & SHIFT_CTRL) && ascii >= 'a' && ascii <= 'z') {
        ascii = ascii - 'a' + 1;
    }

    /* バッファに格納: 全メイクキーイベントを格納（修飾キーは上で既にreturn済み）*/
    if (kbd_count < KBD_BUF_SIZE) {
        u16 entry = ((u16)keycode << 8) | ascii;
        kbd_buf[kbd_tail] = entry;
        kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
        kbd_count++;
    } else {
        /* 満杯なら新しい打鍵を捨てて数える (契約 T3)。WM が
         * kbd_dropped_count() の差分を dropped に合算し OVERFLOW を立てる。 */
        kbd_dropped++;
    }
}

/* ======================================================================== */
/*  kbd_init — キーボード初期化                                             */
/*  μPD8251Aの初期化とIRQ1有効化                                            */
/* ======================================================================== */
void kbd_init(void)
{
    u8 dummy;

    /*
     * μPD8251A 初期化
     *
     * FreeBSDのPC-98 pckbd.c では init_keyboard() は空関数で、
     * BIOSが既に8251Aを初期化済みであることを前提としている。
     *
     * 我々のベアメタルOSでも、ブートローダ経由でBIOSが起動時に
     * 8251Aを初期化しているため、基本的にはそのまま使える。
     * ただし安全のため、エラーリセットと受信イネーブルのみ行う。
     */

    /* 既存のデータを読み捨て (バッファフラッシュ) */
    while (inp(KBD_CMD) & KBD_STAT_RXRDY) {
        dummy = (u8)inp(KBD_DATA);
    }
    (void)dummy;

    /* コマンド: エラーリセット(D4=1) + 受信イネーブル(D2=1) */
    outp(KBD_CMD, KBD_CMD_ERRRST_RXE);

    /* バッファクリア */
    kbd_head = 0;
    kbd_tail = 0;
    kbd_count = 0;
    kbd_dropped = 0;
    kbd_shift_state = 0;
    kbd_gui_mode = 0;       /* 起動直後は CUI (WM 未登録) */

    /* キー状態ビットマップクリア */
    {
        int i;
        for (i = 0; i < 16; i++) kbd_key_pressed[i] = 0;
        kbd_raw_head = kbd_raw_tail = kbd_raw_count = 0;
    }

    /* キーボードIRQを有効化 */
    irq_enable(KBD_IRQ);
}

/* ======================================================================== */
/*  公開API                                                                */
/* ======================================================================== */

int kbd_has_key(void)
{
    return kbd_count > 0;
}

int kbd_trygetchar(void)
{
    u16 entry;

    /* ホストからのファイル配置要求は「シェルが応答している」ときだけ
     * 反映する。exec_nest_level==1 がシェル、2 以上は子プロセス。
     * 子プロセスの文脈で VFS を触ると、そのプログラムが持つ状態と
     * 干渉しうるので踏み込まない。要求が無ければ magic 判定だけで戻る。
     * 設計: docs/tasks/hotdeploy/DESIGN.md */
    if (exec_nest_level <= 1) {
        hotdeploy_poll();
    }

    /* rshellモード: シリアル入力もチェック */
    if (rshell_active) {
        int sch;
        sch = serial_trygetchar();
        if (sch >= 0) return sch;
    }
    
    if (kbd_count == 0) return -1;

    RING_DEQUEUE(entry, kbd_buf, kbd_head, kbd_count, KBD_BUF_SIZE);

    return (int)(entry & 0xFF);
}

int kbd_getchar(void)
{
    u16 entry;
    u32 timeout_ticks;

    /* rshellモード: KBD_TIMEOUT_TICKS タイムアウト (デフォルト300 ticks @ 100Hz) */
    timeout_ticks = rshell_active ? KBD_TIMEOUT_TICKS : 0;

    {
        u32 waited = 0;

        for (;;) {
            /* キーボードバッファ */
            if (kbd_count > 0) {
                RING_DEQUEUE(entry, kbd_buf, kbd_head, kbd_count,
                             KBD_BUF_SIZE);
                return (int)(entry & 0xFF);
            }

            /* rshellモード: シリアル入力もチェック */
            if (rshell_active) {
                int sch;
                sch = serial_trygetchar();
                if (sch >= 0) return sch;
            }

            __asm__ volatile("hlt");

            /* rshellタイムアウト: スペースキーを自動返却 */
            if (timeout_ticks > 0) {
                waited++;
                if (waited >= timeout_ticks) return ' ';
            }
        }
    }
}

/* u16キーコードを返す (上位=スキャンコード, 下位=ASCII) */
int kbd_getkey(void)
{
    u16 entry;
    while (kbd_count == 0) {
        __asm__ volatile("hlt");
    }

    RING_DEQUEUE(entry, kbd_buf, kbd_head, kbd_count, KBD_BUF_SIZE);

    return (int)entry;
}

/* ノンブロッキング版: キーコードデータ(u16)を返す。なければ-1 */
int kbd_trygetkey(void)
{
    u16 entry;

    /* rshellモード: シリアル入力もチェック */
    if (rshell_active) {
        int sch;
        sch = serial_trygetchar();
        if (sch >= 0) return sch; /* シリアルはASCIIのみ(下位バイト) */
    }

    if (kbd_count == 0) return -1;

    RING_DEQUEUE(entry, kbd_buf, kbd_head, kbd_count, KBD_BUF_SIZE);

    return (int)entry;  /* 上位=キーコード, 下位=ASCII */
}

/* 修飾キー(Ctrl/Shift/Alt等)の押下状態を取得 */
u32 kbd_get_modifiers(void)
{
    return (u32)kbd_shift_state;
}

/* 指定スキャンコードのキーが現在押されているかを返す (1=押下中, 0=離されている) */
int kbd_is_pressed(int scancode)
{
    if (scancode < 0 || scancode > 127) return 0;
    return (kbd_key_pressed[scancode >> 3] >> (scancode & 7)) & 1;
}

/* 生 make/break イベントを 1 件取り出す (レビュー ⑥)。無ければ -1。
 * 戻り値 = keycode | (down << 8)。down=1 が押下 (make)、0 が離し (break)。
 * WM (gshell) が Key down/up イベントを作るのに使う。 */
int kbd_trygetrawkey(void)
{
    u16 entry;
    if (kbd_raw_count == 0) return -1;
    RING_DEQUEUE(entry, kbd_raw_buf, kbd_raw_head, kbd_raw_count, KBD_BUF_SIZE);
    return (int)entry;
}

/* ======================================================================== */
/*  kbd_set_gui_mode — GUI (WM 常駐) モードの切替 (K2-B、W1 申し送り ①)     */
/*                                                                          */
/*  kernel/gui.c が gui_register で 1、gui_owner_exit(owner 1) で 0 にする。 */
/*  KAPI は増やさない (GUI の状態はカーネル内で完結する)。                    */
/*  切替のたびに cooked リングを空にする:                                     */
/*    - GUI へ入るとき: CUI で溜まっていた打鍵を捨てる (WM は raw から同じ    */
/*      打鍵を既に受け取っているので二重にはしない)。                        */
/*    - CUI へ戻るとき: GUI 中に (この関数が 0 にする前に) 積まれた分を       */
/*      シェルのプロンプトへ流し込まない。                                    */
/*  捨てた分は kbd_dropped に数えない — 経路の切替であって取りこぼしでは      */
/*  ないため (数えると WM 側に偽の OVERFLOW が出る)。                        */
/*  head/tail/count は IRQ1 と競合するので irq_save で囲む。                  */
/* ======================================================================== */
void kbd_set_gui_mode(int on)
{
    int next = on ? 1 : 0;
    unsigned int flags;

    if (next == kbd_gui_mode) return;

    flags = irq_save();
    kbd_gui_mode = next;
    kbd_head  = 0;
    kbd_tail  = 0;
    kbd_count = 0;
    irq_restore(flags);
}

/* 待ち行列が満杯で捨てた打鍵の累計を返す (契約 T3、GUI v1.1 の KAPI)。 */
u32 kbd_dropped_count(void)
{
    return kbd_dropped + kbd_raw_dropped;
}
