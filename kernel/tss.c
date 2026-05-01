/* ======================================================================== */
/*  TSS.C - Task State Segment 初期化                                       */
/*                                                                          */
/*  V86モードの前提条件としてTSSを構築する。                                */
/*  - ESP0/SS0: V86→Ring0遷移時のカーネルスタック                          */
/*  - I/Oビットマップ: V86タスクのI/Oポートアクセス制御                     */
/*                                                                          */
/*  GDTへのTSSディスクリプタ登録は gdt.c 側で行う。                         */
/* ======================================================================== */

#include "tss.h"
#include "kstring.h"

/* TSSインスタンス (グローバル、GDTから参照される) */
struct tss_entry kernel_tss;

/* ====================================================================== */
/*  tss_init — TSS構造体の初期化                                          */
/*  esp0: Ring0復帰時のスタックポインタ (カーネルスタックトップ)            */
/* ====================================================================== */
void tss_init(u32 kernel_esp0)
{
    /* 全フィールドをゼロクリア */
    kmemset(&kernel_tss, 0, sizeof(struct tss_entry));

    /* Ring0 復帰用スタック (V86→Ring0遷移時にCPUが使用) */
    kernel_tss.ss0  = 0x10;         /* カーネルデータセグメント */
    kernel_tss.esp0 = kernel_esp0;

    /* I/Oビットマップのオフセット (TSS先頭からのバイトオフセット) */
    kernel_tss.iomap_base = (u16)((u32)&kernel_tss.iomap - (u32)&kernel_tss);

    /* 初期状態: 全ポートをトラップ (ビット=1) */
    tss_iomap_deny_all();

    /* I/Oビットマップ終端バイト (Intel仕様: 0xFF必須) */
    kernel_tss.iomap[TSS_IOMAP_SIZE] = 0xFF;
}

/* ====================================================================== */
/*  tss_set_esp0 — Ring0スタックポインタの更新                             */
/* ====================================================================== */
void tss_set_esp0(u32 esp0)
{
    kernel_tss.esp0 = esp0;
}

/* ====================================================================== */
/*  I/Oビットマップ操作                                                    */
/* ====================================================================== */

/* ポートを許可 (ビット=0: V86タスクが直接アクセス可能) */
void tss_iomap_allow(u16 port)
{
    kernel_tss.iomap[port / 8] &= ~(1 << (port % 8));
}

/* ポートを拒否 (ビット=1: アクセス時に#GP発生) */
void tss_iomap_deny(u16 port)
{
    kernel_tss.iomap[port / 8] |= (1 << (port % 8));
}

/* 範囲指定で許可 */
void tss_iomap_allow_range(u16 start, u16 end)
{
    u16 p;
    for (p = start; p <= end; p++) {
        tss_iomap_allow(p);
        if (p == 0xFFFF) break;  /* オーバーフロー防止 */
    }
}

/* 全ポートを拒否 */
void tss_iomap_deny_all(void)
{
    kmemset(kernel_tss.iomap, 0xFF, TSS_IOMAP_SIZE);
}
