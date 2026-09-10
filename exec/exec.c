#include "exec.h"
#include "appslot.h"
#include "exec_heap.h"
#include "io.h"
#include "console.h"
#include "kstring.h"
#include "vfs.h"
#include "gfx.h"
#include "gfx_hal.h"   /* gfx_bb_phys_range: CPL=3 へ USER マップする範囲 */
#include "kbd.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "paging.h"
#include "pgalloc.h"
#include "shlib.h"
#include "fd_redirect.h"
#include "pipe_buffer.h"
#include "shm.h"
#include "gui.h"
#include "snd_engine.h"
#include "kapi_db.h"
#include "gdt.h"
#include "tss.h"

extern void shell_print(const char *s, u8 attr);
extern void shell_print_dec(u32 val, u8 color);
extern u32 sys_mem_kb;
/* kernel/tss.c の TSS 実体。CPL=3 遷移で TSS.ESP0 を現在のカーネル ESP に
 * 合わせる (割り込み/int 0x80 のフレームが exec_run の frame を踏まないよう)。*/
extern struct tss_entry kernel_tss;
static KernelAPI *kapi;

/* ======================================================================== */
/*  KAPI トランポリン (v2 M2)                                                */
/*                                                                          */
/*  CPL=3 アプリは本物の KAPI 表 (カーネルコードポインタ) を読めない/呼べない */
/*  ので、全 PD 共有の USER ページ 1 枚に「本物と同一レイアウトのユーザ可視表 */
/*  + スタブ列」を置き、exec はアプリにこのページのアドレスを渡す。アプリの   */
/*  api->kprintf(...) は表のスタブを呼び、スタブが int 0x80 でカーネルに入る。 */
/*  カーネル band (.bss, PDE0 共有) に置き PTE を RO+USER にする              */
/*  (CR0.WP=0 なのでカーネルは RO でも書ける = per-launch のデータ更新可)。   */
/*  レイアウト (CONTRACTS C3, KernelAPI と同一オフセット):                    */
/*    0x00 magic / 0x04 version / 0x08+ 表[i]=STUB_BASE+i*8 /                */
/*    データフィールド (値) / STUB_BASE: 各 8B スタブ B8<slot>CD80C3         */
/* ======================================================================== */
static u8  ring3_tramp_raw[PAGE_SIZE * 2];   /* 4KB アライン用に 2 ページ分 */
static u32 ring3_tramp_page = 0;             /* 4KB 境界に揃えた実アドレス (=物理) */
static void ring3_trampoline_init(void);

/* int 0x80 引数コピー+呼び出しの ASM ヘルパ (kernel/ring3_entry.asm)。
 * args_src から nbytes をスタックへコピーして wrapfn を cdecl 呼び出し、
 * 戻り値 (eax) を返す。 */
extern u32 kapi_invoke(void *wrapfn, const void *args_src, u32 nbytes);

/* CPL=3 由来のフォールト/不正 slot でアプリを kill (定義は下方, v2 M1e/M2d) */
void ring3_fault_kill(void);

void exec_init(void) {
    kapi = (KernelAPI *)KAPI_ADDR;
    /* アプリ ID の表を空にし、シェル帯 (ID 1) を走っている状態にする。
     * res_owner_set(1) もここで行われる (票 K5 の D3)。 */
    appslot_init();
#include "exec_kapi_init.inc"
    /* 共有メモリ先頭アドレスを公開する。
     * MEM_SHM_BASE はカーネルの __bss_end 由来で可変のため、
     * ユーザ空間側がアドレスをハードコードしてはならない。 */
    kapi->shm_base = (u32)MEM_SHM_BASE;

    /* CPL=3 用 KAPI トランポリンページを構築 (paging_init 済みが前提) */
    ring3_trampoline_init();
}

/* ======================================================================== */
/*  ring3_trampoline_init — トランポリンページの構築 (v2 M2b)               */
/* ======================================================================== */
static void ring3_trampoline_init(void)
{
    u32 page = ((u32)ring3_tramp_raw + PAGE_SIZE - 1) & ~(u32)(PAGE_SIZE - 1);
    u32 *tbl = (u32 *)page;
    u32 stub_base = (page + sizeof(KernelAPI) + 3u) & ~3u; /* 全 struct の後ろ */
    u32 i;

    ring3_tramp_page = page;

    /* magic / version は本物と同じ値 */
    tbl[0] = kapi->magic;
    tbl[1] = kapi->version;

    for (i = 0; i < KAPI_FUNC_COUNT; i++) {
        u8 *st = (u8 *)(stub_base + i * 8u);
        /* ユーザ可視表: entry[i] = スタブ i の番地 (KernelAPI fn[i] と同一 offset) */
        tbl[2 + i] = stub_base + i * 8u;
        /* スタブ: B8 <slot:imm32> CD 80 C3  (mov eax,slot; int 0x80; ret) */
        st[0] = 0xB8;
        st[1] = (u8)(i & 0xFF);
        st[2] = (u8)((i >> 8) & 0xFF);
        st[3] = (u8)((i >> 16) & 0xFF);
        st[4] = (u8)((i >> 24) & 0xFF);
        st[5] = 0xCD;   /* int */
        st[6] = 0x80;   /* 0x80 */
        st[7] = 0xC3;   /* ret */
    }

    /* データフィールド (値): KernelAPI 表と同一オフセット (fn 表の直後)。
     * index 2+KAPI_FUNC_COUNT = sbrk_heap_limit, +1 = shm_base。
     * sbrk_heap_limit は exec_run が launch 時に上書きする。 */
    tbl[2 + KAPI_FUNC_COUNT + 0] = 0;
    tbl[2 + KAPI_FUNC_COUNT + 1] = (u32)MEM_SHM_BASE;

    /* 全 PD 共有で RO+USER マップ (kernel band PDE0)。i386 は NX なしなので
     * RO でも実行可能 (スタブ実行 OK)。ユーザは書けない = スタブ改竄不可。
     * CR0.WP=0 によりカーネルは RO でも書ける (per-launch のデータ更新)。 */
    paging_set_page(page, page, PAGE_RO | PTE_USER);
}

/* スタックを4バイト境界に揃えるためのマスク */
#define STACK_ALIGN_MASK 3

/* 動的確保リザーブ (1MB)。
 *
 * 子プロセスの空間はコード+ヒープ+スタックで pgalloc の管理域
 * (0x400000〜mem_end) をほぼ使い切る。かつてはコード 1MB しか
 * pgalloc_mark_used していなかったため、子の実行中に v86_mem_setup() の
 * pgalloc_alloc_n(160) が「空いている」ヒープ領域 0x500000〜 を確保して
 * 636KB を memset(0) し、起動元プログラムのヒープを破壊していた。
 *
 * 対策: 子の exec ヒープをこのぶんだけ縮め、ヒープ末尾とスタックガードの
 * 間に pgalloc 専用の穴を残す。V86 バッキング RAM (160 ページ = 640KB
 * 連続) はここから取れる。 */
#define EXEC_DYN_RESERVE  (256UL * PAGE_SIZE)

/* ======================================================================== */
/*  コンテキストは exec/appslot.{h,c} の AppSlot 表 (K5b、票 D2/I14)         */
/*                                                                          */
/*  かつては「ネスト段のスタック」(ExecContext exec_ctx_stack[]) だったが、  */
/*  GUI アプリを 4 本同時に生かすには段では足りない — 生きているのは 4 本    */
/*  でも、走っているのは 1 本、残りは OP_WAIT の中で止まっている。           */
/*  よって **ID (1 = シェル帯 / 2〜5 = アプリ) で引く表** に置き換えた。      */
/*  空き ID を必ず小さい方から配るので、CUI の入れ子 exec_run では           */
/*  従来どおり 段 = ID になる (D3)。                                         */
/*                                                                          */
/*  シェル常駐モデル (レイアウトは 1 バイトも変わっていない):                */
/*    ID 1 (シェル): 0x300000 に常駐。CPL=0、AS は作らない                    */
/*    ID 2〜5 (子) : 0x500000 にロード。CPL=3 なら **アプリごとの物理**を     */
/*                   固定仮想 0x500000〜 へ写す (D1)。--cpl0 の子は従来の     */
/*                   アイデンティティのまま (D7)。                           */
/* ======================================================================== */

/* ======================================================================== */
/*  グローバル状態                                                          */
/* ======================================================================== */
/* いま走っているプログラムの段。ID ではなく **深さ** で、シェル = 1。
 * 外 (drivers/kbd.c, kernel/isr_handlers.c) は「> 0 ならプログラムが走って
 * いる」としてしか見ないので意味は変わらない。 */
volatile int exec_nest_level = 0;
volatile int exec_exit_status = EXEC_SUCCESS;

/* longjmp の理由。exec_start / exec_resume の復帰点が park と終了を
 * 見分けるために使う (D4)。exec_run は終了しか受け取らない。 */
#define EXEC_LJ_EXIT   1
#define EXEC_LJ_PARK   2
static volatile int g_longjmp_reason = EXEC_LJ_EXIT;
static volatile int g_longjmp_id = 0;      /* park した ID (resume の戻り値) */

/* いま処理中の int 0x80 フレーム (ring3_syscall_dispatch が控える)。
 * exec_park はこれを AppSlot へ写して CPL=3 の続きを保存する (D2 の (b))。 */
static u32 *g_cur_frame = 0;

/* ======================================================================== */
/*  リング3 (CPL=3) 実行状態 (v2 M1)                                        */
/*                                                                          */
/*  M1 は単一アプリのみ (リング3 のネストは後続)。CPL=3 で走るアプリの      */
/*  アドレス空間を 1 つだけ保持する。int 0x80 (sys_exit) の C 側ディスパッチ */
/*  がここを参照して master PD へ戻し AS を破棄する。                        */
/* ======================================================================== */

/* リング3 ユーザスタック: アプリ帯 (APP_BAND_PDE から始まる帯) の上端に置く
 * (M1_RING3 §5)。プログラム (code + sbrk + exec_heap) は MEM_EXEC_LOAD_ADDR
 * からスタックガード直下まで (レイアウトは include/memmap.h の子プロセス帯の
 * 説明を参照)。K3 でロードアドレスが 1MB 上がったが、**帯の上端は
 * MEM_EXEC_LOAD_ADDR から導かない** — 帯そのものの定数から導く。
 *
 * 2026-09-10 (票 docs/tasks/memory/APP_BAND_PDE.md): 帯の上端は固定ではなく
 * 「アプリ固有 PDE の枚数 × 4MB」。枚数は exec_run がヘッダの heap_size から
 * 決める (paging_app_band_pdes)。heap_size を指定しないプログラムは必ず
 * 1 枚 = 従来と完全に同じレイアウトになる。
 *
 * RING3_USTACK_TOP 以下は**実行時の値**を返すマクロ。定数式が要る文脈
 * (配列長・static 初期化子・case ラベル) では使えないので注意。 */
#define RING3_USTACK_TOP     g_ring3_band_top
/* ユーザスタックサイズ。旧 CPL=0 子プロセスの MEM_EXEC_STACK_SIZE (256KB) に
 * 合わせる (ring3 デフォルト化での深いスタック使用の回帰を避ける)。
 * スタック帯 [上端-256KB, 上端) はプログラム帯 (0x500000-) と
 * 共有ライブラリ帯 (0x400000-0x4FFFFF) より十分上。 */
#define RING3_USTACK_SIZE    MEM_EXEC_STACK_SIZE

/* ヒープとスタックの間に 1 ページのガードを挟む (v2 M3 ハードニング)。
 * ヒープのオーバーラン / スタックのアンダーフローがガード(非present)に当たり
 * #PF → ring3_fault_kill でアプリのみ kill。相互の静かな破壊を防ぐ。 */
#define RING3_GUARD_SIZE     PAGE_SIZE
#define RING3_STACK_BOTTOM   (RING3_USTACK_TOP - RING3_USTACK_SIZE)
#define RING3_GUARD_BASE     (RING3_STACK_BOTTOM - RING3_GUARD_SIZE)
#define RING3_HEAP_TOP       RING3_GUARD_BASE   /* heap 上限=ガード直下 */

/* 帯を最大まで伸ばしたときの上端 / ヒープ上限 (定数)。
 * ファイル読み込みの上限を決めるのに使う — 実際の枚数はヘッダを読むまで
 * 決まらないので、読み込み段階では最大側で見積もる。 */
#define RING3_USTACK_TOP_MAX MEM_APP_BAND_MAX_TOP
#define RING3_HEAP_TOP_MAX   (RING3_USTACK_TOP_MAX - RING3_USTACK_SIZE - \
                              RING3_GUARD_SIZE)

/* 現在の (= いま起動中/実行中の) CPL=3 アプリのアプリ帯上端。
 * 既定は 1 枚ぶん = MEM_APP_BAND_TOP で、CPL=3 アプリが居ない間は必ずこの値。
 * ring3_ptr_ok / argv 積み / USER 写像がすべてここを見るので、
 * 起動失敗・fault kill・正常終了のいずれでも必ず既定へ戻すこと。 */
static u32 g_ring3_band_top = MEM_APP_BAND_TOP;
static u32 g_ring3_band_pdes = 1;

static void ring3_band_set(u32 pdes)
{
    if (pdes < 1) pdes = 1;
    if (pdes > MEM_APP_BAND_MAX_PDES) pdes = MEM_APP_BAND_MAX_PDES;
    g_ring3_band_pdes = pdes;
    g_ring3_band_top = MEM_APP_BAND_BASE + pdes * MEM_APP_BAND_PDE_SIZE;
}

/* いま走っている CPL=3 アプリのスロット (0 = 居ない)。かつての
 * g_ring3_as / g_ring3_active を 1 本にまとめたもの。park すると 0 になり、
 * resume で戻る。生きているだけで走っていないアプリは表に横たわっている。 */
static AppSlot *g_cur_app = 0;

/* CPL=3 アプリをフォールト (#PF/#GP) で kill した回数 (CONTRACTS C6, v2 M1e)。
 * static にせずカーネルシンボルとして公開する (kselftest_pass 等と同じ形)。
 * PM の V4 検証が emu_read_mem で読む。 */
volatile u32 fault_kill_count = 0;

/* sbrk 物理の二段構え (決裁 2026-09-11) の観測点。KAPI にはしない —
 * fault_kill_count と同じくカーネルシンボルを emu_read_mem で読む。
 *   exec_sbrk_tier_last  : 直近の CPL=3 起動が採った段 (1 = 従来式 / 2 = 最低分)
 *   exec_sbrk_tier_count : 段ごとの累計 ([0] = 段 1、[1] = 段 2)
 * 数えるのは 3 領域を実際に張り終えた起動だけ (途中で失敗したものは数えない)。*/
volatile u32 exec_sbrk_tier_last = 0;
volatile u32 exec_sbrk_tier_count[2] = { 0, 0 };

/* ring3 syscall (wrap) 実行中フラグ (v2 M2e フォールトガードの核)。
 * dispatcher が kapi_invoke を挟む間だけ立てる。この間に #PF/#GP が起きたら
 * (wrap 内 = CPL=0 でも) カーネル停止でなくアプリだけ kill する。可変長 %s の
 * ような静的に検証できないポインタ deref もこれで捕捉でき [ABI4] を塞ぐ。
 * 非 static (isr_handlers.c が extern で参照)。 */
volatile int ring3_in_syscall = 0;

/* ======================================================================== */
/*  GUI 入力ポンプと強制脱出 (K2 / 契約 T6・T8 の X4)                        */
/* ======================================================================== */

/* IRQ0 が 100Hz で加算するティック (kernel/idt.h)。ポンプの上限判定は
 * これを **読むだけ** で行う (票 K2 の鉄則: get_tick を叩く回数を増やさない)。 */
extern volatile u32 tick_count;

/* ポンプ実行中フラグ (票 K2-3 の再入防止)。ポンプ自身は CPL=0 の WM コード
 * なので int 0x80 は経由しないが、将来ポンプの中から何かが syscall 境界を
 * 通っても二重取り込みにならないようにカーネル側でも止める。 */
static volatile int g_gui_pump_busy = 0;

/* 最後にポンプを回した tick と、その値が有効かどうか。
 * 「同じ tick の間は 1 回だけ」= 前回から 1 tick (10ms) 未満なら呼ばない。 */
static u32 g_gui_pump_tick = 0;
static int g_gui_pump_tick_valid = 0;

/* CPL=3 アプリの強制脱出要求 (CTRL+STOP、契約 T6)。IRQ1 の kbd_irq_handler が
 * 立て、(a) IRQ1 スタブ (割り込まれた文脈が CPL=3 = アプリのコード実行中の
 * とき) と (b) syscall 入口 が見て ring3_fault_kill する。カーネル内 (wrap の
 * 実行中) では畳まない — 中途半端なカーネル状態で longjmp しないため、
 * 要求は残して次の安全な地点で処理する。
 *
 * K5b: 要求は **走っているアプリの AppSlot** に立てる (D4)。IRQ1 の時点で
 * カーネルが知っているのはそれだけで、止めてあるアプリには届かない
 * (そちらは WM が exec_kill で畳む)。 */

/* CTRL+STOP で畳んだ回数 (PM の V4 検証が emu_read_mem で読む)。
 * fault_kill_count にも含まれる (畳む経路は同じ ring3_fault_kill)。 */
volatile u32 ring3_abort_count = 0;

/* ======================================================================== */
/*  ring3_abort_request — CTRL+STOP を受けた (IRQ1 の ISR から呼ばれる)      */
/*                                                                          */
/*  ISR の中では畳まない (EOI も V86 反射も済んでいない)。CPL=3 アプリが      */
/*  走っているときだけ要求を立てる。CUI でシェルしか居ないときは無視。        */
/* ======================================================================== */
void ring3_abort_request(void)
{
    appslot_abort_request();
}

/* ======================================================================== */
/*  ring3_abort_check — 要求があればアプリを畳む (戻らない)                  */
/*                                                                          */
/*  呼び出し元は 2 か所:                                                     */
/*    - kernel/isr_stub.asm の IRQ1 スタブ (EOI と V86 反射の後、割り込まれた */
/*      文脈が CPL=3 のときだけ)。KAPI を呼ばない計算ループはここで死ぬ。     */
/*    - ring3_syscall_dispatch の入口 (wrap に入る前)。                      */
/*  ring3_fault_kill は master CR3 復帰 → AS 破棄 → longjmp で戻らない。      */
/*  longjmp 先の exec_run は復帰点で _enable() するので、割り込みゲート経由で */
/*  IF=0 のまま来ても割り込みは戻る。                                        */
/* ======================================================================== */
void ring3_abort_check(void)
{
    AppSlot *a = appslot_get(appslot_cur());
    if (!a || !a->abort_req) return;
    a->abort_req = 0;
    if (!g_cur_app) return;         /* CPL=3 アプリはもう居ない */
    ring3_abort_count++;
    ring3_fault_kill();             /* 戻らない */
}

/* ======================================================================== */
/*  ring3_gui_pump — syscall 境界の入力ポンプ (票 K2-1/2/3、契約 T6)         */
/*                                                                          */
/*  アプリが KAPI を呼んでいる限り、WM (gshell) が登録したポンプをここで      */
/*  回し、キーボードとマウスの入力を取りこぼさないようにする。               */
/*                                                                          */
/*  上限 (票 K2-1):                                                          */
/*    - syscall 1 回につき最大 1 回 (この関数はディスパッチャから 1 回だけ    */
/*      呼ばれる)。                                                          */
/*    - 前回から 1 tick (10ms) 未満なら呼ばない。KAPI を毎秒数百回叩く        */
/*      アプリ (v2 PLAN §3 の実測 233/s) で WM の処理が支配的にならないため。 */
/*                                                                          */
/*  除外 (票 K2-3): WM 自身 (owner 1 = シェル帯) からの呼び出し。WM は自分の  */
/*  周期 (X3 = OP_WAIT) で回すので、ここで二重に回さない。                    */
/*                                                                          */
/*  呼ぶ位置は kapi_invoke の **前**、かつフォールトガード                    */
/*  (ring3_in_syscall) の **外側**。ポンプは CPL=0 の WM コードで、ここで      */
/*  落ちるのはアプリのせいではない (契約 T8 の X4: 落ちたらカーネルの責任)。  */
/*  ポンプ実行中も IF=1 のまま — IRQ は普通に入る (int80_stub が sti 済み)。  */
/* ======================================================================== */
static void ring3_gui_pump(void)
{
    GuiPump pump;
    u32 now;

    if (g_gui_pump_busy) return;             /* 再入防止 (票 K2-3) */

    pump = (GuiPump)gui_get_pump();
    if (pump == 0) return;                   /* WM 未登録 (CUI) */

    if (res_owner_get() == GUI_SHELL_OWNER) return;  /* WM 自身の syscall */

    now = tick_count;
    if (g_gui_pump_tick_valid && now == g_gui_pump_tick) {
        return;                              /* 同じ tick 内 = 1 tick 未満 */
    }
    g_gui_pump_tick = now;
    g_gui_pump_tick_valid = 1;

    g_gui_pump_busy = 1;
    pump();
    g_gui_pump_busy = 0;
}

/* ユーザポインタ引数の早期範囲検証 (v2 M2e 補助)。exec が CPL=3 アプリに
 * USER マップした領域 (共有ライブラリ帯/プログラム帯/ユーザスタック/SHM/VRAM)
 * と NULL のみ許可。範囲外 (例: 0xDEADBEEF) は wrap に入る前に弾き、
 * カーネル状態不整合を避ける。
 * 可変長引数はここでは見えないのでフォールトガードが担保する。 */
static int ring3_ptr_ok(u32 p)
{
    if (p == 0) return 1;                         /* NULL は wrap 側が処理 */
    if (p >= MEM_SHLIB_BASE && p < RING3_HEAP_TOP) return 1;
        /* 共有ライブラリ帯 (K3: .rodata の文字列や .data の構造体を KAPI に
         * 渡せる。.text への **書き込み** は PTE が RO なのでハードウェアの
         * #PF で捕まる — ここは「番地として正しいか」だけを見る) +
         * アプリの code/data/bss/heap (ガード直下まで) */
    if (p >= RING3_STACK_BOTTOM && p < RING3_USTACK_TOP) return 1;
        /* ユーザスタック帯。ガードページ [RING3_GUARD_BASE, RING3_STACK_BOTTOM)
         * は不許可 (ここを指すポインタは早期検証で kill)。 */
    if (p >= (u32)MEM_SHM_BASE &&
        p <  (u32)MEM_SHM_BASE + (u32)MEM_SHM_SIZE) return 1;  /* SHM */
    if (p >= 0xA0000UL && p < 0xC0000UL) return 1;/* VRAM (テキスト/グラフィック) */
    return 0;
}

#include "ksetjmp.h"

/* ======================================================================== */
/*  exec_child_claim — 子プロセスが占有する物理ページ範囲を求める            */
/*                                                                          */
/*  範囲 A: コード + guard_a + exec ヒープ (動的確保リザーブの手前まで)      */
/*  範囲 B: guard_b + スタック (mem_end まで)                               */
/*  A と B の間の穴 (EXEC_DYN_RESERVE) は pgalloc の動的確保用に残す。       */
/*                                                                          */
/*  mark (exec_run) と free (setjmp 復帰) の双方から同じ式で計算する。       */
/*  グローバル (sys_mem_kb) と定数だけから求めるのは、setjmp 後に書き換わる  */
/*  ローカル変数を longjmp 復帰側で参照しないため。                          */
/* ======================================================================== */
static void exec_child_claim(u32 *a_start, int *a_pages,
                             u32 *b_start, int *b_pages)
{
    u32 mem_end = sys_usable_mem_end();  /* 末尾はホットデプロイ用に予約 */
    u32 guard_b = mem_end - MEM_EXEC_STACK_SIZE - PAGE_SIZE;
    u32 a_end = guard_b;

    if (guard_b > MEM_EXEC_LOAD_ADDR &&
        (guard_b - MEM_EXEC_LOAD_ADDR) > EXEC_DYN_RESERVE * 2) {
        a_end = guard_b - EXEC_DYN_RESERVE;
    }
    *a_start = MEM_EXEC_LOAD_ADDR;
    *a_pages = (int)((a_end - MEM_EXEC_LOAD_ADDR) / PAGE_SIZE);
    *b_start = guard_b;
    *b_pages = (int)((mem_end - guard_b) / PAGE_SIZE);
}

/* ======================================================================== */
/*  ring3_band_ram_top — アプリ帯を伸ばしてよい物理上限                      */
/*                                                                          */
/*  子プロセスの claim 範囲 A の末尾。そこまでは exec_child_claim が          */
/*  pgalloc に予約させるので、帯を伸ばしてもアプリのヒープと pgalloc の       */
/*  動的確保 (V86 バッキング・PD/PT) が同じページを二重に使うことがない。     */
/*  8MB 構成では帯 1 枚ぶんにも届かないが、paging_app_band_pdes() が          */
/*  最低 1 枚を返すので従来の挙動 (帯 = 0x400000-0x7FFFFF) は変わらない。     */
/* ======================================================================== */
static u32 ring3_band_ram_top(void)
{
    /* K5b (D1/I4): CPL=3 アプリはもう [0x500000, mem_end) を丸ごと押さえない。
     * 物理は pgalloc から必要枚数だけ取るので、帯を伸ばしてよい上限は
     * 「実 RAM の上端」そのもの。入らなければ pgalloc が失敗し、exec が
     * EXEC_ERR_NOMEM で拒否する (切り詰めない・スワップしない、D5)。 */
    return sys_usable_mem_end();
}


/* CPL=0 の子 (mkos32x --cpl0) は従来どおり identity で走り、固定帯
 * [MEM_EXEC_LOAD_ADDR, mem_end) を exec_child_claim で押さえる (D7)。
 * CPL=3 アプリはもう押さえないので、claim を返してよいのは
 * **CPL=0 の子が 1 本も居なくなったとき**。段の深さでは決められない
 * (GUI アプリが CPL=0 の子を持てるため) ので本数で数える。 */
static int g_cpl0_children = 0;

static void exec_cpl0_claim(void)
{
    u32 ca_start, cb_start;
    int ca_pages, cb_pages;
    if (g_cpl0_children++ > 0) return;
    exec_child_claim(&ca_start, &ca_pages, &cb_start, &cb_pages);
    pgalloc_mark_used(ca_start, ca_pages);
    pgalloc_mark_used(cb_start, cb_pages);
}

static void exec_cpl0_release(void)
{
    u32 ca_start, cb_start;
    int ca_pages, cb_pages;
    if (g_cpl0_children <= 0) return;
    if (--g_cpl0_children > 0) return;
    exec_child_claim(&ca_start, &ca_pages, &cb_start, &cb_pages);
    pgalloc_free_n(ca_start, ca_pages);
    pgalloc_free_n(cb_start, cb_pages);
}

/* ======================================================================== */
/*  sbrk 物理の二段構え (ユーザー決裁 2026-09-11)                            */
/*                                                                          */
/*  K5b-K で per-app 物理にしたとき、heap_size 未指定の CPL=3 プログラムの    */
/*  sbrk に張る物理を最低分 (MEM_EXEC_SBRK_MIN = 256KB) へ固定した。8MB       */
/*  構成で GUI アプリ 1 本を通すためだったが、identity だった頃は帯の残り     */
/*  ぜんぶ (≒1.4MB) が黙って sbrk に使えたので、malloc を多用する CUI        */
/*  プログラム (less 等) が割を食う。                                        */
/*                                                                          */
/*  そこで二段構えにする:                                                    */
/*    段 1 = 従来式。sbrk 上端を guard_a まで伸ばす ([code_end, guard_a) 全部)*/
/*    段 2 = 最低分。sbrk 上端を code_end + MEM_EXEC_SBRK_MIN に落とす        */
/*  段 1 で 3 領域 (本体+sbrk / exec_heap / スタック) + PD + アプリ PT が     */
/*  pgalloc の空きに収まるなら段 1、収まらなければ段 2。段 2 でも収まらない   */
/*  ときは呼び出し側が EXEC_ERR_NOMEM を返す (切り詰めない・スワップしない)。 */
/*                                                                          */
/*  heap_size を明示したプログラムはこの分岐に入らない (K5b-K のまま最低分)。 */
/*  要求した exec_heap を必ず渡すのが先で、sbrk を伸ばす余地はそこに無い。    */
/*  どちらの段で走ったかは exec_sbrk_tier_last / exec_sbrk_tier_count[] で    */
/*  後から読める (KAPI にはしない。fault_kill_count と同じカーネルシンボル)。 */
/* ======================================================================== */
static u32 exec_ring3_pages(u32 load_base, u32 sbrk_end, u32 exec_heap_size,
                            u32 band_pdes)
{
    return (sbrk_end - load_base) / PAGE_SIZE       /* 本体 + sbrk */
         + exec_heap_size / PAGE_SIZE               /* exec_heap */
         + RING3_USTACK_SIZE / PAGE_SIZE            /* ユーザスタック */
         + 1 + band_pdes;                           /* PD + アプリ PT */
}

/* 選んだ段 (1 or 2) を返し、*sbrk_end に sbrk の上端を書く。 */
static int exec_sbrk_pick_tier(u32 load_base, u32 code_end, u32 guard_a,
                               u32 exec_heap_size, u32 band_pdes,
                               u32 free_pages, u32 *sbrk_end)
{
    u32 lo = code_end + MEM_EXEC_SBRK_MIN;

    if (lo > guard_a) lo = guard_a;
    if (exec_ring3_pages(load_base, guard_a, exec_heap_size, band_pdes)
            <= free_pages) {
        *sbrk_end = guard_a;
        return 1;
    }
    *sbrk_end = lo;
    /* 帯の残りが最低分より狭いなら、落としても従来式と同じものを張っている。 */
    return (lo >= guard_a) ? 1 : 2;
}

/* ======================================================================== */
/*  app_map_region — アプリ帯の 1 領域を per-app 物理で張る (D1)             */
/*                                                                          */
/*  まず連続で取り (P1 の map_user_range_phys)、断片化で取れなければ          */
/*  ページ単位に倒す。per-app 物理にした副産物で連続は必須ではない —          */
/*  K5a の申し送り D10 が「断片化が出たらページ単位へ倒せ」と書いた点。       */
/*  途中で尽きたら -1。張り終えたぶんは呼び出し側の巻き戻し                   */
/*  (exec_teardown_app → paging_addrspace_free_user_range) が PTE を辿って   */
/*  返すので、ここで部分解放はしない。                                        */
/* ======================================================================== */
static int app_map_region(struct addrspace *as, u32 vstart, u32 vend)
{
    u32 pages, phys, v;

    if (vstart >= vend) return 0;
    pages = (vend - vstart) / PAGE_SIZE;

    phys = pgalloc_alloc_n((int)pages);
    if (phys) {
        if (paging_addrspace_map_user_range_phys(as, vstart, vend, phys,
                                                 PAGE_RW | PTE_USER) == 0) {
            return 0;
        }
        pgalloc_free_n(phys, (int)pages);
        return -1;
    }

    for (v = vstart; v < vend; v += PAGE_SIZE) {
        phys = pgalloc_alloc_page();
        if (!phys) return -1;
        if (paging_addrspace_map_user(as, v, phys, PAGE_RW | PTE_USER) != 0) {
            pgalloc_free_page(phys);
            return -1;
        }
    }
    return 0;
}

/* ======================================================================== */
/*  exec_teardown_app — CPL=3 アプリの物理とアドレス空間を返す (D1/D4)       */
/*                                                                          */
/*  **master CR3 に戻してから**呼ぶこと (破棄する PD がアクティブだと         */
/*  paging_addrspace_destroy が何もせずに戻る)。                             */
/*  返すのはこのアプリ帯の 3 領域だけ:                                        */
/*    [load_addr, sbrk_heap_limit)        本体 + data + bss + sbrk (最低分)   */
/*    [exec_heap_base, +exec_heap_size)   exec_heap                          */
/*    [band_top - stack, band_top)        ユーザスタック                     */
/*  ガードページ (guard_a / guard_b) は「張っていない = 非 present」なので     */
/*  返すものが無い。共有帯 (VRAM / SHM / フォント / GFX / トランポリン) は     */
/*  paging_addrspace_free_user_range がアプリ固有 PDE の外を触らないので       */
/*  巻き添えにならない。 */
/* ======================================================================== */
static void exec_teardown_app(AppSlot *a)
{
    if (!a || !a->cpl3 || !a->as.pd_phys) return;
    /* 共有ライブラリの .data 複製ページを返す (PD 破棄の前, K3) */
    shlib_addrspace_detach(&a->as);
    if (a->sbrk_heap_limit > a->load_addr)
        paging_addrspace_free_user_range(&a->as, a->load_addr,
                                         a->sbrk_heap_limit);
    if (a->exec_heap_size)
        paging_addrspace_free_user_range(&a->as, a->exec_heap_base,
                                         a->exec_heap_base + a->exec_heap_size);
    if (a->band_top > RING3_USTACK_SIZE)
        paging_addrspace_free_user_range(&a->as,
                                         a->band_top - RING3_USTACK_SIZE,
                                         a->band_top);
    paging_addrspace_destroy(&a->as);
    a->cpl3 = 0;
}

/* ======================================================================== */
/*  exec_restore_context — 「現在のプログラム」を id のものに切り替える       */
/*                                                                          */
/*  票 D1 の I7 / I9 / I10 — 仮想レイアウトは動かさず、カーネル側の           */
/*  「いま走っているのは誰か」を表す値だけを差し替える:                       */
/*    CR3 / アプリ帯の上端 / exec_heap の管理変数 / sbrk 上限 (本物の表と      */
/*    トランポリンの両方) / exec ネスト段。                                   */
/*  CPL=0 のプログラム (シェル / --cpl0 の子) のガードは master の identity    */
/*  ページなので張り直す。CPL=3 アプリのガードはアプリ PT ごと捨てるので       */
/*  何もしない (I8)。                                                        */
/* ======================================================================== */
static void exec_restore_context(int id)
{
    AppSlot *a = appslot_at(id);
    if (!a) return;

    if (a->cpl3 && a->as.pd_phys) {
        g_cur_app = a;
        ring3_band_set(a->band_pdes);
        paging_load_cr3(a->as.pd_phys);
    } else {
        g_cur_app = 0;
        ring3_band_set(1);
        paging_load_cr3(paging_kernel_pd_phys());
    }

    if (a->exec_heap_base != 0) {
        /* exec_heap_init_at ではなく restore_state。init_at はヒープ先頭に
         * 空きブロックヘッダを書き直してしまい、親が子の起動前に確保して
         * いたブロックのヘッダを壊す ("bad magic feeefeee" の正体)。 */
        exec_heap_restore_state(a->exec_heap_base, a->exec_heap_size,
                                a->exec_heap_used);
    }
    kapi->sbrk_heap_limit = a->sbrk_heap_limit;
    if (a->cpl3) {
        ((u32 *)ring3_tramp_page)[2 + KAPI_FUNC_COUNT + 0] = a->sbrk_heap_limit;
    }
    if (!a->cpl3 && a->guard_a != 0) {
        paging_set_not_present(a->guard_a, a->guard_a + PAGE_SIZE - 1);
        paging_set_not_present(a->guard_b, a->guard_b + PAGE_SIZE - 1);
    }
    exec_nest_level = a->depth;
}

/* ======================================================================== */
/*  exec_reclaim_owned — この ID が持っている資源だけを回収する (D3)         */
/*                                                                          */
/*  かつては「exec のネスト段」で回していた並びを、そのまま **アプリ ID** で  */
/*  回すようにしたもの。7 種のうち shm と db は所有者を見ていなかったので     */
/*  shm_free_owned / db_cleanup_owned へ差し替えてある (D3 の表、P3/P5)。     */
/*  ここを段のまま残すと、アプリ A の終了がアプリ B の SHM や DB 接続を       */
/*  巻き上げる (4 本同時では実際に起きる)。                                   */
/* ======================================================================== */
static void exec_reclaim_owned(int id)
{
    /* (1) 標準FDのリダイレクト解除 (ファイルFDも自動クローズ)。
     * 表は FD 0/1/2 の 3 本しかないので、2 本のアプリが同時に stdout を
     * リダイレクトすることはできない (D3 の限界。GUI アプリは使わない)。 */
    fd_redirect_reset_owned(id);
    /* (2) FD自動クローズ (この ID が open した FD 3 以上)。
     * カーネル常駐FD (vfs_fd_set_protect で保護) は除外される。 */
    vfs_close_owned(id);
    /* (3) パイプバッファ自動解放 */
    pipe_free_owned(id);
    /* (4) 共有メモリ (P3: 所有者付きになった) */
    shm_free_owned(id);
    /* (5) サウンド: この ID の退避済み音だけを捨てる (D9-4)。
     * 鳴っているのがこの ID なら止める。他のアプリの音は無事。 */
    snd_owner_exit(id);
    /* (6) SQLite DB リソース (P5: cleanup_all → cleanup_owned) */
    db_cleanup_owned(id);
    /* (7) GUI リソース回収 (契約 T4 / U8)。WM がこの owner のウィンドウ・
     * サーフェス・タイマ・スロットを回収する。畳む 3 経路すべてが
     * ここを通るので、WM は 1 か所で回収できる。 */
    gui_owner_exit(id);
}

/* ======================================================================== */
/*  exec_launch_abort — 起動途中で失敗したときの唯一の巻き戻し口             */
/*                                                                          */
/*  K5b で構成が単純になった: 起動は **最後まで失敗しうる操作を済ませてから** */
/*  appslot_start_commit する (= owner / 段 / ヒープの切り替えは iret の      */
/*  直前 1 か所) ので、ここで戻すのは 3 つだけ:                              */
/*    (1) アプリの物理ページとアドレス空間 (取れていれば)                     */
/*    (2) CR3 と「現在のプログラム」を起動元へ                               */
/*    (3) スロットを空へ                                                     */
/*  起動失敗では所有者回収を回さない — 子のコードは 1 命令も走っておらず、    */
/*  この ID のタグを持つ資源は存在し得ない (模型ケース 10)。                  */
/* ======================================================================== */
static int exec_launch_abort(int launcher_id, int id, int status)
{
    AppSlot *a = appslot_at(id);
    if (a) {
        /* スロットはまだ commit していない (state は FREE のまま) ので、
         * 返すのはアプリの物理とアドレス空間だけ。回収カウンタも動かさない。 */
        paging_load_cr3(paging_kernel_pd_phys());
        exec_teardown_app(a);
        a->pages = 0;
    }
    exec_restore_context(launcher_id);
    return status;
}

/* 起動を諦めるときに「アプリ帯の上端」だけを起動元の値へ戻す。
 * exec_restore_context を使うと exec_heap の管理変数まで巻き戻してしまい、
 * まだ save していない起動元のヒープ使用量が古い値で上書きされる。 */
static void exec_restore_band(int id)
{
    AppSlot *a = appslot_at(id);
    ring3_band_set((a && a->cpl3) ? a->band_pdes : 1);
}

/* longjmp する側がスロットを空にするので、jmpbuf は先に控える。 */
static u32 g_exit_jmpbuf[KSETJMP_BUF_LEN];

/* ======================================================================== */
/*  exec_exit — 現在のプログラムを畳み、その ID の呼び出し元へ戻る            */
/*                                                                          */
/*  畳むのは常に「いま走っている 1 本」だけ (D4)。正常終了・fault・          */
/*  CTRL+STOP の 3 経路が全部ここを通る。 */
/* ======================================================================== */
void exec_exit(int status)
{
    int id = appslot_cur();
    AppSlot *a = appslot_get(id);
    int parent;
    u32 k;

    if (!a) return;
    exec_exit_status = status;

    /* 後始末とシェル復帰は master PD 上で行う。 */
    if (g_cur_app) {
        paging_load_cr3(paging_kernel_pd_phys());
    }

    /* CPL=0 のプログラムだけがカーネルの identity ページを触っている。
     * CPL=3 アプリのガードとヒープはアプリ PT ごと捨てるので不要 (I8)。 */
    if (!a->cpl3) {
        if (a->guard_a != 0) {
            paging_set_page(a->guard_a, a->guard_a, PAGE_RW);
            paging_set_page(a->guard_b, a->guard_b, PAGE_RW);
        }
        if (a->exec_heap_base != 0 && id != APP_ID_SHELL) {
            exec_heap_reset();
        }
    }

    for (k = 0; k < KSETJMP_BUF_LEN; k++) g_exit_jmpbuf[k] = a->jmpbuf[k];

    /* **回収より先に**親の文脈へ戻す。回収の最後に呼ぶ gui_owner_exit() は
     * WM (gshell) のコードで、そこで KAPI の mem_alloc を踏むと exec_heap が
     * 「畳んだアプリの仮想ヒープ」を指したままになる — その物理はもう
     * pgalloc へ返しているので、master CR3 の下で他人のページを書きに行く。
     * 回収は全部 ID を明示して呼ぶので、owner を先に戻しても取りこぼさない。 */
    if (id == APP_ID_SHELL) {
        /* シェル自身の終了 (K4 のシェル起動ループへ戻る)。従来どおり
         * 段 0 / owner 0 に落として exec_run(shell) の setjmp 点へ帰る。 */
        g_cur_app = 0;
        ring3_band_set(1);
        exec_nest_level = 0;
        res_owner_set(0);
        exec_reclaim_owned(id);
    } else {
        if (!a->cpl3) exec_cpl0_release();
        parent = appslot_return_target(id);
        exec_teardown_app(a);
        appslot_reclaim(id);
        /* 終了に伴う master 復帰は「生存アプリの集合が変わる瞬間」= G7 の
         * 切替ではない。appslot_switch_to が transition_count で別勘定する。 */
        appslot_switch_to(parent);
        exec_restore_context(parent);
        exec_reclaim_owned(id);
    }

    g_longjmp_reason = EXEC_LJ_EXIT;
    g_longjmp_id = 0;
    exec_longjmp(g_exit_jmpbuf);
}

void exec_fault_recover(void)
{
    exec_exit(EXEC_ERR_FAULT);
}

void __cdecl kapi_sys_exit(int status)
{
    /* CPL=3 (リング3) アプリからの正常終了 (トランポリン経由, v2 M2)。
     * master CR3 復帰・AS 破棄・per-app 物理の返却は exec_exit が ID 単位で
     * 行う。CPL=0 プログラム (シェル等) は g_cur_app が 0 なので従来どおり。 */
    ring3_in_syscall = 0;   /* syscall(sys_exit) を抜ける — ガードを下ろす */
    exec_exit(status);
}

/* ======================================================================== */
/*  ring3_syscall_dispatch — CPL=3 からの int 0x80 ディスパッチャ (v2 M2d)   */
/*                                                                          */
/*  kernel/ring3_entry.asm の int80_stub が pushad 後のフレーム先頭を渡す。  */
/*  フレーム (u32 配列, pushad + CPU が積んだ例外フレーム):                   */
/*    [0..7]=pushad (EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX)  → EAX=[7]             */
/*    [8]=EIP [9]=CS [10]=EFLAGS [11]=userESP [12]=userSS                    */
/*                                                                          */
/*  eax(=[7]) がスタブの積んだ slot。範囲外は即 kill (CONTRACTS C4)。        */
/*  本物の KAPI 表 (KAPI_ADDR: [magic][version][fn0..]) から wrap を引き、    */
/*  ユーザスタック (userESP+4, スタブの ret アドレス分を飛ばす) から引数を    */
/*  コピーして呼ぶ。戻り値は eax スロット([7])へ書く → popad で復元される。   */
/*  現 CR3 はアプリ PD のまま呼ぶ (ユーザポインタ引数がアプリ帯で解決される)。*/
/*  sys_exit は wrap → kapi_sys_exit が teardown+longjmp するのでここへ戻らない。*/
/*                                                                          */
/*  K5b: フレーム先頭を g_cur_frame に控える。gshell の op_wait が            */
/*  exec_park() を呼んだとき、この 13 語をそのまま AppSlot へ写して           */
/*  「CPL=3 の続き」を保存する (D2 の (b): カーネルスタックは 1 本のまま)。   */
/* ======================================================================== */

/* 可変長引数 (kprintf) を拾うためのコピー窓 (固定分より広めに取る)。 */
#define RING3_ARG_WINDOW  64u

void __cdecl ring3_syscall_dispatch(u32 *frame)
{
    u32 slot     = frame[7];         /* スタブが積んだ slot (eax) */
    u32 user_esp = frame[11];        /* CPL=3 の ESP (int が積んだ) */
    const void *args_src;
    u32 nbytes;
    u32 window;
    u32 wrapptr;
    u32 *prev_frame = g_cur_frame;

    g_cur_frame = frame;

    /* --- CTRL+STOP の要求があればここで畳む (契約 T6) --- */
    ring3_abort_check();        /* 要求があれば longjmp して戻らない */

    /* --- GUI 入力ポンプ (票 K2-1、契約 T6 / T8 の X4) ---
     * フォールトガード (ring3_in_syscall) を立てる **前** に回す。 */
    ring3_gui_pump();

    /* 範囲外 slot はワイルド呼び出し → アプリだけ kill (カーネルを飛ばさない)。 */
    if (slot >= (u32)KAPI_FUNC_COUNT) {
        ring3_fault_kill();
    }

    /* 本物の表から wrap_<slot> を取得 ([magic][version] の後が fn 表)。 */
    wrapptr = ((const u32 *)KAPI_ADDR)[2 + slot];
    args_src = (const void *)(user_esp + 4u);
    nbytes = (u32)kapi_argsize[slot];

    /* --- (核) フォールトガードを立てる (v2 M2e) --- */
    ring3_in_syscall = 1;

    /* --- (補助) 明示ポインタ引数の早期範囲検証 (v2 M2e) --- */
    {
        u16 ptrmask = kapi_argptr[slot];
        if (ptrmask) {
            const u32 *a = (const u32 *)args_src;
            u32 nfixed = nbytes / 4u;   /* 固定引数の個数 */
            u32 k;
            for (k = 0; k < nfixed && k < 16u; k++) {
                if ((ptrmask & (u16)(1u << k)) && !ring3_ptr_ok(a[k])) {
                    ring3_fault_kill();   /* 範囲外ポインタ → kill、戻らない */
                }
            }
        }
    }

    /* 引数コピー窓: 固定分 + 可変長(kprintf)のため広めに取り、ユーザスタック
     * 上端でクランプして over-read #PF を避ける。 */
    window = (nbytes < RING3_ARG_WINDOW) ? RING3_ARG_WINDOW : nbytes;
    if ((u32)args_src < RING3_USTACK_TOP &&
        (u32)args_src + window > RING3_USTACK_TOP) {
        window = RING3_USTACK_TOP - (u32)args_src;
    }

    /* 本物の wrap を呼ぶ (現 CR3 = アプリ PD)。戻り値を eax スロットへ。
     * gui_call(OP_WAIT) → exec_park() はここから longjmp して戻らない。 */
    frame[7] = kapi_invoke((void *)wrapptr, args_src, window);

    /* 正常復帰: ガードを下ろす */
    ring3_in_syscall = 0;
    g_cur_frame = prev_frame;

    /* --- 出口でも CTRL+STOP を見る (契約 T6、v1.2 G2 で実測した隙間) --- */
    ring3_abort_check();
}

/* ======================================================================== */
/*  ring3_fault_kill — CPL=3 由来の #PF/#GP でアプリを kill (v2 M1e)        */
/*                                                                          */
/*  #PF/#GP ハンドラ (kernel/isr_handlers.c) がフォールトフレームの         */
/*  CS.RPL=3 (= CPL=3 由来) を検出したときに呼ぶ。カーネルを巻き込まず       */
/*  **その ID だけ**を畳んでシェル (WM) に戻す (D4)。                        */
/*  後始末は正常終了と同一 — exec_exit が master CR3 復帰 → per-app 物理の   */
/*  返却 → AS 破棄 → ID 別回収 → longjmp までを 1 か所で行う。               */
/*  この関数は longjmp するので戻らない。                                    */
/* ======================================================================== */
void ring3_fault_kill(void)
{
    fault_kill_count++;
    ring3_in_syscall = 0;   /* syscall 途中で畳む場合も必ずガードを下ろす */
    exec_fault_recover();   /* longjmp するので戻らない */
}

/* ======================================================================== */
/*  ring3_resume — 保存した CPL=3 フレームへ戻る (P4、kernel/ring3_entry.asm)*/
/*  cli → TSS.ESP0 → CR3 → フレームを積んで popad; iretd を割り込み禁止で    */
/*  一続きに行う。戻らない。                                                 */
/* ======================================================================== */
extern void ring3_resume(const u32 *frame, u32 pd_phys, void *tss);

/* ======================================================================== */
/*  exec_launch — 外部プログラムのロードと実行 (exec_run / exec_start の実体) */
/*                                                                          */
/*  gui = 0: 従来の exec_run。子が終わるまで呼び出し元を塞ぐ。               */
/*  gui = 1: K5b の exec_start。子が最初の OP_WAIT で park した時点でも戻る。 */
/*                                                                          */
/*  違いは **どこで longjmp を受けるか** の 1 点だけで、ロードもレイアウトも  */
/*  共通。仮想レイアウト (app.ld / memmap.h の RING3_*) は 1 バイトも         */
/*  動かない (I12/I13) — 動いたのは「物理をどこから取るか」だけ。            */
/* ======================================================================== */
static int exec_launch(const char *cmdline, int gui_arg)
{
    /* longjmp の復帰側で読む唯一のローカル。volatile でフレーム上に固定する
     * — レジスタに置かれると longjmp で失われる (他は全部グローバルで判断)。 */
    volatile int gui = gui_arg;
    u32 load_base;
    u32 max_size;
    u32 stack_top;
    u32 guard_a, guard_b;
    u32 exec_heap_base, exec_heap_size;
    u32 sbrk_end;            /* 実際に物理を張る sbrk の上端 (= sbrk 上限) */
    int sbrk_tier = 0;       /* sbrk 物理の段 (1 = 従来式 / 2 = 最低分、0 = 非CPL3) */
    u32 heap_top_cpl0 = 0;
    int is_shell;
    int launcher_id;
    int id;
    u32 need_pages;

    u32 mem_end = sys_usable_mem_end();  /* 末尾はホットデプロイ用に予約 */
    u8 *file_buf;
    u8 *load_addr;
    OS32Header *hdr;
    int sz;
    u32 code_off, text_sz, bss_sz, heap_sz, entry_off;
    ExecEntry entry;
    AppSlot *ctx;
    int want_ring3 = 0;      /* CPL=3 で走らせるか (OS32X_FLAG_RING3, v2 M1) */

    char path[VFS_MAX_PATH];
    /* 解決済みのパス。ヘッダを先に 1 ページ読むので、本体の読み込みでは
     * 同じ探索をやり直さずこちらを使う (探索でヒットした綴りを保つ)。 */
    static char resolved[VFS_MAX_PATH];
    /* ヘッダだけを先に読むカーネル側バッファ。本体を読む先の物理は、
     * ヘッダの text_size / bss_size / heap_size を見るまで決まらない
     * (D1 の「起動時の順序」手順 2)。 */
    static u8 hdrbuf[OS32X_HDR_V2_SIZE + 64];
    const char *p = cmdline;
    int i = 0;

    launcher_id = appslot_cur();
    is_shell = (exec_nest_level == 0);

    /* ---- ID の池 (D3)。物理の勘定はヘッダを読んでから ---- */
    if (is_shell) {
        id = APP_ID_SHELL;
    } else {
        id = appslot_start_admit(gui, 0, 0);
        if (id < 0) {
            if (id == OS32_ERR_FULL)
                shell_print("Error: too many programs running\n", ATTR_RED);
            return id;
        }
    }

    /* コマンドラインからパスを抽出 */
    while (*p == ' ') p++;
    while (*p && *p != ' ' && i < (int)sizeof(path) - 1) {
        path[i++] = *p++;
    }
    path[i] = '\0';

    /* ====== Level に応じたメモリレイアウト決定 ====== */
    if (is_shell) {
        /* シェル: 常駐帯域 0x300000-0x37FFFF */
        load_base = MEM_SHELL_LOAD_ADDR;
        max_size  = MEM_SHELL_MAX_SIZE;
        stack_top = MEM_SHELL_STACK_TOP;
        guard_a   = 0; /* シェルは sbrk/exec_heap 未使用 */
        guard_b   = MEM_SHELL_GUARD;
        sbrk_end  = 0;
        exec_heap_base = 0;
        exec_heap_size = 0;
    } else {
        /* 子プロセス: 0x500000〜。guard_a / exec_heap はヘッダを見てから。 */
        u32 child_stack_bottom;
        u32 read_top;
        load_base = MEM_EXEC_LOAD_ADDR;
        stack_top = mem_end;
        child_stack_bottom = stack_top - MEM_EXEC_STACK_SIZE;
        guard_b   = child_stack_bottom - PAGE_SIZE;
        heap_top_cpl0 = guard_b;
        if (guard_b > MEM_EXEC_LOAD_ADDR &&
            (guard_b - MEM_EXEC_LOAD_ADDR) > EXEC_DYN_RESERVE * 2) {
            heap_top_cpl0 = guard_b - EXEC_DYN_RESERVE;
        }
        read_top = (heap_top_cpl0 < RING3_HEAP_TOP_MAX) ?
                   heap_top_cpl0 : RING3_HEAP_TOP_MAX;
        max_size = read_top - load_base - MEM_EXEC_SBRK_MIN - PAGE_SIZE - MEM_EXEC_HEAP_MIN;
        guard_a = 0;
        sbrk_end = 0;
        exec_heap_base = 0;
        exec_heap_size = 0;
    }

    load_addr = (u8 *)load_base;

    /* ====== ヘッダだけ先読み (master CR3、カーネルバッファ) ======
     * 現行のように全部読んでから枚数を決めることはできない — 読む先の
     * 物理がまだ無いため (D1 の手順 2)。 */
    kstrncpy(resolved, path, VFS_MAX_PATH);
    sz = vfs_read(resolved, hdrbuf, (int)sizeof(hdrbuf));

    /* フォールバック: パスにスラッシュがない場合、標準ディレクトリを順に検索 */
    /* 注意: SYS_DEFAULT_PATH (config.h) と整合させること */
    if (sz <= 0) {
        int has_slash = 0;
        int pi;
        for (pi = 0; path[pi]; pi++) {
            if (path[pi] == '/') { has_slash = 1; break; }
        }
        if (!has_slash) {
            static const char *search_dirs[] = {
                "/bin/", "/sbin/", "/usr/bin/", (const char *)0
            };
            int di;
            for (di = 0; search_dirs[di]; di++) {
                kstrncpy(resolved, search_dirs[di], VFS_MAX_PATH);
                kstrncat(resolved, path, VFS_MAX_PATH);
                sz = vfs_read(resolved, hdrbuf, (int)sizeof(hdrbuf));
                if (sz > 0) break;
            }
        }
    }

    if (sz <= 0) {
        return EXEC_ERR_NOT_FOUND;
    }

    hdr = (OS32Header *)hdrbuf;

    if (hdr->magic != OS32X_MAGIC || hdr->header_size < OS32X_HDR_V1_SIZE ||
        hdr->min_api_ver > KAPI_VERSION) {
        shell_print("Error: invalid OS32X binary\n", ATTR_RED);
        return EXEC_ERR_INVALID;
    }

    /* ---- ロードアドレスの照合 (K3) ---- */
    if (!is_shell) {
        if (hdr->version < OS32X_HDR_VERSION ||
            hdr->header_size < OS32X_HDR_V2_SIZE) {
            shell_print("Error: old OS32X binary (no load_addr) - rebuild required\n",
                        ATTR_RED);
            return EXEC_ERR_INVALID;
        }
        if (hdr->load_addr == 0) {
            kprintf(0xE1, "[exec] warning: %s has no load_addr\n", path);
        } else if (hdr->load_addr != load_base) {
            kprintf(0xC1, "[exec] load addr mismatch: bin=%x expected=%x\n",
                    hdr->load_addr, load_base);
            shell_print("Error: OS32X load address mismatch - rebuild required\n",
                        ATTR_RED);
            return EXEC_ERR_INVALID;
        }
    }

    code_off  = hdr->header_size;
    text_sz   = hdr->text_size;
    bss_sz    = hdr->bss_size;
    heap_sz   = hdr->heap_size;
    entry_off = hdr->entry_offset;

    /* v2 M3a: ring3 をデフォルト化。シェルは CPL=0 のまま。それ以外の全
     * プログラムを CPL=3 で起動する。稀に CPL=3 で動かせないものは
     * OS32X_FLAG_FORCE_CPL0 (mkos32x --cpl0) で CPL=0 に落とす。 */
    want_ring3 = appslot_launch_is_app(is_shell, hdr->flags);
    if (want_ring3) {
        u32 code_end_est = PAGE_ALIGN_UP(load_base + text_sz + bss_sz);
        ring3_band_set(paging_app_band_pdes(code_end_est, heap_sz,
                                            ring3_band_ram_top()));
        stack_top = RING3_USTACK_TOP;
    } else if (appslot_cpl0_admit(is_shell) < 0) {
        /* --cpl0 の子はアプリ帯を丸ごと identity で押さえる (D7)。生きている
         * CPL=3 アプリの per-app 物理を上書きし、終了時に他人のページを
         * 解放してしまうので、1 本でも居たら起動しない (決裁 2026-09-11)。
         * exec_cpl0_claim() より前 — claim も alloc もまだ何もしていない。 */
        shell_print("Error: close GUI apps before running a --cpl0 program\n",
                    ATTR_RED);
        return OS32_ERR_FULL;
    }

    if (!is_shell) {
        /* 子プロセス帯のレイアウト確定 (include/memmap.h 参照):
         *   [load..code_end) 本体 / [code_end..guard_a) sbrk / [guard_a] ガード /
         *   [exec_heap_base..heap_top) exec_heap */
        u32 heap_top = want_ring3 ? RING3_HEAP_TOP : heap_top_cpl0;
        u32 code_end = (load_base + text_sz + bss_sz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        u32 need = MEM_EXEC_SBRK_MIN + PAGE_SIZE + MEM_EXEC_HEAP_MIN;
        u32 avail;

        if (heap_top < code_end || heap_top - code_end < need) {
            shell_print("[DBG] NOMEM: text=", 0xE1);
            shell_print_dec(text_sz, 0xE1);
            shell_print(" bss=", 0xE1);
            shell_print_dec(bss_sz, 0xE1);
            shell_print(" max=", 0xE1);
            shell_print_dec((heap_top > load_base + need) ? heap_top - need - load_base : 0, 0xE1);
            shell_print("\n", 0xE1);
            exec_restore_band(launcher_id);   /* 帯を起動元の値へ戻す */
            return EXEC_ERR_NOMEM;
        }
        avail = heap_top - code_end - MEM_EXEC_SBRK_MIN - PAGE_SIZE;
        if (heap_sz > 0) {
            exec_heap_size = (heap_sz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (exec_heap_size < MEM_EXEC_HEAP_MIN) exec_heap_size = MEM_EXEC_HEAP_MIN;
            /* 要求に足りないときは**黙って切り詰めず拒否する** (2026-09-10 方針)。*/
            if (exec_heap_size > avail) {
                shell_print("[DBG] NOMEM: heap request=", 0xE1);
                shell_print_dec(heap_sz, 0xE1);
                shell_print(" avail=", 0xE1);
                shell_print_dec(avail, 0xE1);
                shell_print("\n", 0xE1);
                exec_restore_band(launcher_id);
                return EXEC_ERR_NOMEM;
            }
        } else {
            exec_heap_size = (avail / 2) & ~(PAGE_SIZE - 1);
            if (exec_heap_size < MEM_EXEC_HEAP_MIN) exec_heap_size = MEM_EXEC_HEAP_MIN;
        }
        exec_heap_base = heap_top - exec_heap_size;
        guard_a = exec_heap_base - PAGE_SIZE;

        /* sbrk に **物理を張る**範囲を決める。per-app 物理では張ったぶんしか
         * 無いので、張らない [sbrk_end, guard_a) は穴のままにし、sbrk 上限も
         * sbrk_end に下げる — こうすると足りないとき newlib の sbrk が素直に
         * 失敗し (malloc が NULL を返す)、静かな #PF にならない。
         * CPL=0 の子は従来どおり帯を丸ごと identity で押さえるので guard_a。 */
        if (!want_ring3) {
            sbrk_end = guard_a;
        } else if (heap_sz > 0) {
            /* heap_size 明示は K5b-K のまま最低分に固定 (段の分岐なし)。 */
            sbrk_end = code_end + MEM_EXEC_SBRK_MIN;
            if (sbrk_end > guard_a) sbrk_end = guard_a;
            sbrk_tier = (sbrk_end >= guard_a) ? 1 : 2;
        } else {
            /* heap_size 未指定は二段構え (決裁 2026-09-11)。 */
            sbrk_tier = exec_sbrk_pick_tier(load_base, code_end, guard_a,
                                            exec_heap_size, g_ring3_band_pdes,
                                            pgalloc_free_pages(), &sbrk_end);
        }
    } else if (text_sz + bss_sz > max_size) {
        shell_print("[DBG] NOMEM: text=", 0xE1);
        shell_print_dec(text_sz, 0xE1);
        shell_print(" bss=", 0xE1);
        shell_print_dec(bss_sz, 0xE1);
        shell_print(" max=", 0xE1);
        shell_print_dec(max_size, 0xE1);
        shell_print("\n", 0xE1);
        return EXEC_ERR_NOMEM;
    }

    /* ======== 物理の勘定 (D5)。入らなければ拒否、切り詰めない ======== */
    need_pages = 0;
    if (want_ring3) {
        need_pages = exec_ring3_pages(load_base, sbrk_end, exec_heap_size,
                                      g_ring3_band_pdes);
        if (appslot_start_admit(gui, need_pages, pgalloc_free_pages()) < 0) {
            shell_print("[DBG] NOMEM: need pages=", 0xE1);
            shell_print_dec(need_pages, 0xE1);
            shell_print(" free=", 0xE1);
            shell_print_dec(pgalloc_free_pages(), 0xE1);
            shell_print("\n", 0xE1);
            exec_restore_band(launcher_id);
            return EXEC_ERR_NOMEM;
        }
    }

    /* ======== 起動元のヒープ使用量を控える (I7) ========
     * ここから先の失敗 (exec_launch_abort) は起動元の状態を戻すので、
     * **失敗しうる操作より前に**控えておく。後ろに置くと、巻き戻しが
     * 古い exec_heap_used で起動元のヒープ管理変数を上書きする。 */
    if (!is_shell) {
        exec_heap_save_state(&appslot_at(launcher_id)->exec_heap_used);
    }

    /* ======== スロットに諸元を書く (まだ commit しない) ======== */
    ctx = appslot_at(id);
    ctx->load_addr = load_base;
    ctx->stack_top = stack_top;
    ctx->guard_a = guard_a;
    ctx->guard_b = guard_b;
    ctx->exec_heap_base = exec_heap_base;
    ctx->exec_heap_size = exec_heap_size;
    ctx->exec_heap_used = 0;
    ctx->sbrk_heap_limit = is_shell ? guard_b : sbrk_end;
    ctx->cpl3 = 0;
    ctx->band_top = g_ring3_band_top;
    ctx->band_pdes = g_ring3_band_pdes;
    ctx->pages = need_pages;

    /* ======== CPL=3: アドレス空間と per-app 物理 (D1) ======== */
    if (want_ring3) {
        u32 bb_base = 0, bb_size = 0;

        if (paging_addrspace_create_n(&ctx->as, g_ring3_band_pdes) != 0) {
            shell_print("Error: ring3 addrspace create failed\n", ATTR_RED);
            exec_restore_band(launcher_id);
            return EXEC_ERR_NOMEM;
        }
        ctx->cpl3 = 1;

        /* **I6**: アプリ PT は master の identity PTE で初期化されている。
         * 落とし忘れると物理 0x5xxxxx が素通しで見え、他アプリのページや
         * pgalloc の作業域が CPL=3 から読める。ここが本設計で最も静かに
         * 壊れる箇所なので、per-app 物理を張る前に必ず全部 0 にする。 */
        paging_addrspace_clear_app_band(&ctx->as);

        /* 3 領域を per-app 物理で張る。連続が取れなければページ単位へ倒す
         * (D10 の断片化の申し送り)。ガードは張らない = 非 present のまま。 */
        if (app_map_region(&ctx->as, load_base, sbrk_end) != 0 ||
            app_map_region(&ctx->as, exec_heap_base,
                           exec_heap_base + exec_heap_size) != 0 ||
            app_map_region(&ctx->as, RING3_STACK_BOTTOM, RING3_USTACK_TOP) != 0) {
            shell_print("Error: out of physical memory for app\n", ATTR_RED);
            return exec_launch_abort(launcher_id, id, EXEC_ERR_NOMEM);
        }

        /* 3 領域を張り終えてから段を記録する (途中で失敗したものは数えない)。*/
        if (sbrk_tier == 1 || sbrk_tier == 2) {
            exec_sbrk_tier_last = (u32)sbrk_tier;
            exec_sbrk_tier_count[sbrk_tier - 1]++;
        }

        /* VRAM (テキスト 0xA0000 + グラフィック 0xA8000) — C2: 全PD共有+USER */
        paging_addrspace_map_user_range(&ctx->as,
            0xA0000UL, 0xC0000UL, PAGE_RW | PTE_USER);
        /* SHM (アプリ間データ受け渡し) — C2: 全PD共有+USER */
        paging_addrspace_map_user_range(&ctx->as,
            (u32)MEM_SHM_BASE, (u32)MEM_SHM_BASE + (u32)MEM_SHM_SIZE,
            PAGE_RW | PTE_USER);
        /* フォントキャッシュ (0x01000-0x49FFF): kcg フォントビットマップ直読 */
        paging_addrspace_map_user_range(&ctx->as,
            (u32)MEM_FONT_CACHE_BASE, (u32)MEM_UNICODE_TABLE_BASE,
            PAGE_RW | PTE_USER);
        /* Unicode-JIS 変換表 (0x4A000, 128KB): unicode_to_jis() 直読 */
        paging_addrspace_map_user_range(&ctx->as,
            (u32)MEM_UNICODE_TABLE_BASE,
            (u32)MEM_UNICODE_TABLE_BASE + (u32)MEM_UNICODE_TABLE_SIZE,
            PAGE_RW | PTE_USER);
        /* 9801 の主記憶バックバッファ (0x6A000, 128KB) は **常に** USER に
         * する (レビュー #6)。Cirrus の setup 失敗で 9801 へ落ちたとき、
         * 最初の CPU 描画が #PF になるのを防ぐ。 */
        paging_addrspace_map_user_range(&ctx->as,
            (u32)MEM_GFX_BB_BASE,
            (u32)MEM_GFX_BB_BASE + (u32)MEM_GFX_BB_SIZE,
            PAGE_RW | PTE_USER);
        /* いま選ばれているバックエンド固有の面を足す。map_user_range では
         * なく **_keep** — Cirrus のクライアント面は PCD 付きのデバイス窓で、
         * flags をそのまま書くと PCD が落ちる (レビュー #5 ②③)。 */
        gfx_bb_phys_range(&bb_base, &bb_size);
        if (bb_size)
            paging_addrspace_map_user_keep(&ctx->as,
                bb_base, bb_base + bb_size, PAGE_RW | PTE_USER);
        /* KAPI トランポリンページ (RO+USER, 全PD共有) */
        paging_addrspace_map_user(&ctx->as, ring3_tramp_page,
            ring3_tramp_page, PAGE_RO | PTE_USER);

        /* --- K3: 共有ライブラリ帯域 (0x400000-0x4FFFFF) ---
         * .text/.rodata は RO+USER、.data/.bss は同じ仮想番地にこのアプリ
         * 専用の物理ページ (原本から複製)。**master CR3 のまま**行う —
         * 原本 g_data_master は共有ライブラリ帯の末尾 = アプリ固有 PDE の
         * 中にあり、アプリ CR3 の下では別物を指す (I11)。 */
        if (shlib_addrspace_attach(&ctx->as) < 0) {
            shell_print("Error: shlib data attach failed (out of memory)\n", ATTR_RED);
            return exec_launch_abort(launcher_id, id, EXEC_ERR_NOMEM);
        }
    }

    /* ======== setjmp — この ID の呼び出し元へ帰る点 ======== */
    if (exec_setjmp(ctx->jmpbuf) != 0) {
        /* ======== longjmp復帰ポイント ========
         * ローカル変数は当てにできない (setjmp 後に書き換わったものが
         * 復帰側では読めない)。判断材料はグローバルだけに限る。
         *
         * フォルト経由の復帰では例外ゲートが IF をクリアしたまま longjmp
         * してくる (exec_longjmp は EFLAGS を復元しない)。呼び出し元は常に
         * 割り込み有効で動いているので、ここで無条件に開けてよい。 */
        _enable();
        /* 畳み (終了 / fault / CTRL+STOP) も park も、戻す作業は
         * exec_exit / exec_park の側で済んでいる。ここは値を返すだけ。 */
        if (g_longjmp_reason == EXEC_LJ_PARK) {
            return g_longjmp_id;      /* app_id (2〜5) — まだ生きている */
        }
        return gui ? 0 : exec_exit_status;
    }

    entry = (ExecEntry)(load_addr + entry_off);

    /* ======== ここから先は「このアプリの文脈」========
     * CR3 をアプリ PD に載せ、本体を読み込み、argv を積む (I1/I2/I3)。
     * VFS / kmalloc / ドライバ / ISR はすべて PDE 0 = 全 PD 共有なので、
     * この間もカーネルは普通に動く。 */
    if (want_ring3) {
        paging_load_cr3(ctx->as.pd_phys);
    } else if (!is_shell) {
        /* CPL=0 の子は従来どおり identity。固定帯を pgalloc に予約させる。 */
        exec_cpl0_claim();
    }

    file_buf = (u8 *)load_base;
    {
        /* 読み込みの上限は **実際に物理を張った範囲** で頭打ちにする。
         * per-app 物理では [sbrk_end, guard_a) は張っていない穴なので、
         * max_size のまま読ませるとカーネル (CPL=0) が穴に書いて #PF になる
         * — アプリの fault ではなくカーネルが飛ぶ。
         * レイアウトの検査が guard_a - code_end >= MEM_EXEC_SBRK_MIN を
         * 保証しているので、ヘッダ + text はこの範囲に必ず収まる。 */
        u32 read_max = max_size + OS32X_HDR_V2_SIZE;
        if (want_ring3 && (sbrk_end - load_base) < read_max) {
            read_max = sbrk_end - load_base;
        }
        sz = vfs_read(resolved, file_buf, (int)read_max);
    }
    if (sz <= 0) {
        if (want_ring3) {
            return exec_launch_abort(launcher_id, id, EXEC_ERR_NOT_FOUND);
        }
        /* claim したのは CPL=0 の**子**だけ (シェルは identity の常駐帯で、
         * exec_cpl0_claim を通っていない)。左右を揃えないと、シェルの
         * 読み込み失敗が子の本数勘定を触る。 */
        if (!is_shell) exec_cpl0_release();
        return EXEC_ERR_NOT_FOUND;
    }

    {
        /* ヘッダ分だけ前方へ詰めるオーバーラップコピー。kmemcpy は
         * オーバーラップ時の動作を保証しないので memmove を使う。 */
        memmove(load_addr, load_addr + code_off, text_sz);
        kmemset(load_addr + text_sz, 0, bss_sz);
    }

    /* ヒープ・ガードページ設定 */
    if (is_shell) {
        /* シェルのヒープは 2 系統あり、領域を分ける (include/memmap.h 参照):
         *   - newlib の sbrk (malloc / stdio バッファ): BSS 終端 〜 guard_b
         *   - KAPI mem_alloc (exec_heap): スタック上の MEM_SHELL_HEAP_BASE 〜 */
        exec_heap_base = MEM_SHELL_HEAP_BASE;
        exec_heap_size = MEM_SHELL_HEAP_SIZE;
        if (heap_sz > 0 && heap_sz < exec_heap_size) exec_heap_size = heap_sz;
        ctx->exec_heap_base = exec_heap_base;
        ctx->exec_heap_size = exec_heap_size;
        kapi->sbrk_heap_limit = guard_b;
    }

    if (exec_heap_size > 0) {
        exec_heap_init_at(exec_heap_base, exec_heap_size);
    }

    if (!is_shell) {
        kapi->sbrk_heap_limit = sbrk_end;
        if (!want_ring3) {
            /* CPL=0 の子のガードは master の identity ページ。CPL=3 アプリの
             * ガードは「アプリ PT に張っていない」= 非 present がそのまま
             * ガードになるので、master を触らない (I8)。 */
            if (paging_set_not_present(guard_a, guard_a + PAGE_SIZE - 1) != 0 ||
                paging_set_not_present(guard_b, guard_b + PAGE_SIZE - 1) != 0) {
                kprintf(0xC1, "[exec] guard page setup failed (a=%x b=%x)\n",
                        guard_a, guard_b);
            }
        }
    }

    {
        char *str_area;
        char **argv_area;
        int argc = 0;
        int cmd_len = kstrlen(cmdline);
        const char *s = cmdline;
        char *d;
        u32 new_esp;
        u32 u_esp;   /* ring3: iret に渡すユーザ ESP (ダミー retaddr 込み) */
        /* 呼び出し元 ESP の退避先。ローカルにしないのは、子のスタックへ
         * 切り替えた後の復帰ムーブが %esp/%ebp 相対では読めないため。
         * ID 別に持つ (単一 static だとネスト exec で上書きされる)。 */
        static u32 saved_esp_stack[APP_SLOT_COUNT];

        stack_top -= (cmd_len + 1);
        stack_top &= ~((u32)STACK_ALIGN_MASK);
        str_area = (char *)stack_top;

        stack_top -= sizeof(char *) * OS32_MAX_ARGS;
        argv_area = (char **)stack_top;

        s = cmdline;
        d = str_area;
        while (*s) {
            char quote;

            /* 引数間の空白をスキップ */
            while (*s == ' ') s++;
            if (!*s) break;

            /* 新しい引数を開始 */
            if (argc < OS32_MAX_ARGS - 1) argv_area[argc++] = d;

            /* クォート対応トークナイザ */
            while (*s && *s != ' ') {
                if (*s == '"' || *s == '\'') {
                    quote = *s++;
                    while (*s && *s != quote) {
                        if (*s == '\\' && quote == '"' && *(s + 1)) {
                            s++;
                        }
                        *d++ = *s++;
                    }
                    if (*s == quote) s++;  /* 閉じクォートをスキップ */
                } else if (*s == '\\' && *(s + 1)) {
                    s++;
                    *d++ = *s++;
                } else {
                    *d++ = *s++;
                }
            }
            *d++ = '\0';
        }
        argv_area[argc] = NULL;

        /* ---- 呼び出しフレームを子スタック上に自分で組む ----
         * ExecEntry は __cdecl (int argc, char **argv, KernelAPI *api) なので
         * 低位から argc, argv, kapi の順に並べる。call 時点で ESP を 16 バイト
         * 境界に揃えるのは SysV i386 ABI の要求。 */
        new_esp = (stack_top - 3 * sizeof(u32)) & ~(u32)15;
        ((u32 *)new_esp)[0] = (u32)argc;
        ((u32 *)new_esp)[1] = (u32)argv_area;
        ((u32 *)new_esp)[2] = (u32)kapi;

        if (want_ring3) {
            /* --- M2c: CPL=3 アプリには本物の表でなくトランポリン表を渡す ---
             * CPL=3 の sbrk 上限は guard_a (exec_heap の直下のガード)。 */
            ((u32 *)ring3_tramp_page)[2 + KAPI_FUNC_COUNT + 0] = sbrk_end;
            ((u32 *)ring3_tramp_page)[2 + KAPI_FUNC_COUNT + 1] = kapi->shm_base;
            ((u32 *)new_esp)[2] = ring3_tramp_page;   /* api = トランポリン */

            /* --- crt0 スタック規約合わせ (retaddr ズレ修正) ---
             * ring3 は iret でエントリへ飛ぶため call が無く retaddr が
             * 積まれない。iret に渡す ESP を argc の 1 スロット下にし、
             * そこにダミー retaddr を置いて call 経路と同一に揃える。 */
            u_esp = new_esp - sizeof(u32);
            ((u32 *)u_esp)[0] = 0;   /* ダミー retaddr */

            g_cur_app = ctx;
        }

        /* ======== ここで初めて「走っているのはこの ID」になる ======== */
        if (is_shell) {
            appslot_shell_commit();
        } else {
            appslot_start_commit(id, gui, need_pages);
        }
        exec_nest_level = ctx->depth;

        if (want_ring3) {
            /* --- M1d: iret で CPL=3 に降りる ---
             * CS=USER_CS(0x23) / SS=USER_DS(0x2B)。EFLAGS=0x202 (IF=1, IOPL=0)。
             * TSS.ESP0 を現在の ESP に設定: CPL=3 実行中の割り込み / int 0x80 の
             * フレームがこの直下に積まれ、setjmp フレームを踏まない。
             * ここから通常 return しない — 終了は int 0x80 → longjmp。 */
            __asm__ volatile(
                "cli\n\t"
                "movl %%esp, %[e0]\n\t"     /* TSS.ESP0 = 現在のカーネル ESP */
                "movl %[pd], %%cr3\n\t"     /* アプリ PD へ切替 */
                "movl %[uds], %%eax\n\t"
                "movw %%ax, %%ds\n\t"
                "movw %%ax, %%es\n\t"
                "movw %%ax, %%fs\n\t"
                "movw %%ax, %%gs\n\t"
                "pushl %[uds]\n\t"          /* SS = USER_DS */
                "pushl %[uesp]\n\t"         /* ESP = ユーザスタック */
                "pushl $0x202\n\t"          /* EFLAGS: IF=1, IOPL=0 */
                "pushl %[ucs]\n\t"          /* CS = USER_CS */
                "pushl %[eip]\n\t"          /* EIP = エントリポイント */
                "iret\n\t"
                : [e0] "=m"(kernel_tss.esp0)
                : [pd]  "r"(ctx->as.pd_phys),
                  [uesp]"r"(u_esp),
                  [eip] "r"((u32)entry),
                  [uds] "i"(USER_DS),
                  [ucs] "i"(USER_CS)
                : "eax", "memory"
            );
            /* iret 後はここへ戻らない */
        } else {
            __asm__ volatile(
                "movl %%esp, %0\n\t"
                "movl %1, %%esp\n\t"
                "call *%2\n\t"
                "movl %0, %%esp"
                : "=m"(saved_esp_stack[id])
                : "r"(new_esp), "r"(entry)
                : "eax", "ecx", "edx", "cc", "memory"
            );
        }
        exec_exit(EXEC_SUCCESS);
    }

    return EXEC_SUCCESS;
}

/* ======================================================================== */
/*  exec_run — 従来どおり「子が終わるまで塞ぐ」起動 (CUI の入れ子はこれ)     */
/* ======================================================================== */
int exec_run(const char *cmdline)
{
    return exec_launch(cmdline, 0);
}

/* ======================================================================== */
/*  exec_start — 塞がない起動 (KAPI v44、D4 / 決裁 D9-5)                     */
/*                                                                          */
/*  戻り値: >0 = app_id (2〜5)。最初の OP_WAIT まで進んで park した          */
/*          0  = park より前に終了した (回収済み、gui_owner_exit 配送済み)   */
/*          <0 = 起動しなかった (OS32_ERR_INVAL / OS32_ERR_FULL /            */
/*               EXEC_ERR_NOMEM / EXEC_ERR_NOT_FOUND / EXEC_ERR_INVALID)     */
/*  owner 1 (シェル帯) からのみ — 判定は gui_register と同じ形 (契約 S2)。    */
/* ======================================================================== */
i32 exec_start(const char *cmdline)
{
    if (res_owner_get() != APP_ID_SHELL) return OS32_ERR_INVAL;
    if (cmdline == 0 || cmdline[0] == '\0') return OS32_ERR_INVAL;
    return exec_launch(cmdline, 1);
}

/* ======================================================================== */
/*  exec_park — 走っているアプリを OP_WAIT の中で止め、WM へ戻す (KAPI v44)  */
/*                                                                          */
/*  成立すれば **戻らない** (longjmp で exec_start / exec_resume の復帰点へ)。*/
/*  呼べない文脈 (OP_WAIT 以外の op / 走っているアプリが居ない / CPL=0 の子) */
/*  では OS32_ERR_INVAL を返して普通に戻り、ring3_park_reject_count が増える。*/
/*                                                                          */
/*  park 規約 (D2): 呼んでよいのは gshell の op_wait のループ先頭、wm_cycle  */
/*  が 1 周を終えた直後・ring::pending を読む前だけ。ここから longjmp する    */
/*  ので、WM が書きかけの状態を持っていると宙に浮く。                        */
/* ======================================================================== */
i32 exec_park(void)
{
    int id = appslot_cur();
    AppSlot *a;
    u32 k;
    int rc;

    rc = appslot_park_check();
    if (rc < 0) return rc;

    a = appslot_get(id);
    /* CPL=3 のフレームが無ければ止めようがない (CPL=0 の子 / 呼び出し文脈が
     * syscall の外)。check を通っていても最後にここで弾く。 */
    if (!a || !g_cur_app || g_cur_app != a || g_cur_frame == 0) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }

    /* CPL=3 の続き = int80_stub のフレーム 13 語。resume はこれを積み直して
     * popad; iretd するだけ (D2 の (b): 追加 RAM は 1 アプリ 52B)。 */
    for (k = 0; k < APP_FRAME_WORDS; k++) a->frame[k] = g_cur_frame[k];

    exec_heap_save_state(&a->exec_heap_used);
    ring3_in_syscall = 0;       /* この syscall はここで終わる */
    g_cur_frame = 0;

    /* master へ戻してから状態を切り替える (WM は master の下で走る)。 */
    paging_load_cr3(paging_kernel_pd_phys());
    appslot_park_commit();      /* PARKED + 印 + owner 1 へ */
    exec_restore_context(APP_ID_SHELL);

    g_longjmp_reason = EXEC_LJ_PARK;
    g_longjmp_id = id;
    exec_longjmp(a->jmpbuf);    /* 戻らない */
    return 0;
}

/* ======================================================================== */
/*  exec_resume — 止めてあるアプリを 1 本だけ起こす (KAPI v44)               */
/*                                                                          */
/*  戻り値: app_id = また park した / 0 = 終了した / <0 = 起こせなかった      */
/*  wait_ret は OP_WAIT の戻り値 (契約 T3: ring::pending)。保存フレームの     */
/*  EAX スロットに書くので、アプリから見れば gui_call(OP_WAIT) が普通に        */
/*  その値を返したように見える。                                             */
/*                                                                          */
/*  起こせるのは **OP_WAIT で park された印のあるフレームだけ** (C5/C6)。     */
/*  印が無ければ OS32_ERR_STALE を返して ring3_resume_bad_frame_count を上げる*/
/*  — WM の行儀を信じるのではなくカーネルが弾く形 (受入 G7)。                 */
/* ======================================================================== */
i32 exec_resume(i32 app_id, i32 wait_ret)
{
    AppSlot *a;
    int rc;

    rc = appslot_resume_check((int)app_id);
    if (rc < 0) return rc;

    a = appslot_get((int)app_id);
    if (!a->cpl3 || !a->as.pd_phys) return OS32_ERR_INVAL;
    a->frame[APP_FRAME_EAX] = (u32)wait_ret;

    if (exec_setjmp(a->jmpbuf) != 0) {
        /* park / 終了 / fault / kill で戻ってきた。ローカルは当てにしない。 */
        _enable();
        if (g_longjmp_reason == EXEC_LJ_PARK) return g_longjmp_id;
        return 0;
    }

    appslot_resume_commit((int)app_id);
    exec_restore_context((int)app_id);
    /* cli → TSS.ESP0 → CR3 → popad; iretd を割り込み禁止で一続きに。
     * iretd が保存済み EFLAGS (IF=1) を復元するのでアプリ側の IF は変わらない。 */
    ring3_resume(a->frame, a->as.pd_phys, &kernel_tss);
    return 0;   /* 到達しない */
}

/* ======================================================================== */
/*  exec_kill — 止めてあるアプリを起こさずに畳む (KAPI v44、決裁 D9-6)       */
/*                                                                          */
/*  CTRL+STOP は「いま走っているアプリ」宛にしか立たない (IRQ1 の時点で       */
/*  カーネルが知っているのはそれだけ) ので、止めてあるアプリを畳む口が別に    */
/*  要る。これが無いと resume されないまま固まったアプリを永久に畳めない。    */
/*  owner 1 (WM top-level) からのみ。走っている本人には OS32_ERR_STALE。      */
/* ======================================================================== */
i32 exec_kill(i32 app_id)
{
    AppSlot *a;
    int rc = appslot_kill_check((int)app_id);
    if (rc < 0) return rc;

    a = appslot_get((int)app_id);
    /* 走っていないので CR3 は master のまま。owner も 1 のまま動かさない
     * — 回収は全部 ID を明示して呼ぶ (D3)。 */
    exec_reclaim_owned((int)app_id);
    if (!a->cpl3) exec_cpl0_release();
    exec_teardown_app(a);
    appslot_reclaim((int)app_id);
    /* 生存アプリの集合が変わる瞬間 = transition。G7 の switch ではない。 */
    ring3_transition_count++;
    return 0;
}

/* ======================================================================== */
/*  exec_app_state — 0=空き / 1=走っている / 2=park 中 (KAPI v44、任意)      */
/* ======================================================================== */
i32 exec_app_state(i32 app_id)
{
    return appslot_state((int)app_id);
}
