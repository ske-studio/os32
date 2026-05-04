/* ======================================================================== */
/*  TSS.H - Task State Segment (タスク状態セグメント) 定義                  */
/*                                                                          */
/*  V86モード (仮想DOS環境) の前提条件。V86→Ring0遷移時にCPUがTSSから       */
/*  ESP0/SS0を取得するため、TSSの存在が必須。                               */
/*                                                                          */
/*  I/Oパーミッションビットマップ:                                         */
/*    ビット=0 → V86タスクがそのポートに直接アクセス可能                    */
/*    ビット=1 → アクセス時に#GP発生、OS32がエミュレート                    */
/* ======================================================================== */

#ifndef TSS_H
#define TSS_H

#include "types.h"

/* I/Oパーミッションビットマップサイズ (65536ポート / 8 = 8192バイト) */
#define TSS_IOMAP_SIZE  8192

/* TSS構造体 (i386仕様: 最小104バイト + I/Oビットマップ) */
struct tss_entry {
    u32 prev_tss;       /* 前のTSSセレクタ (未使用) */
    u32 esp0;           /* Ring0 スタックポインタ */
    u32 ss0;            /* Ring0 スタックセグメント (0x10 = カーネルデータ) */
    u32 esp1;           /* Ring1 ESP (未使用) */
    u32 ss1;            /* Ring1 SS  (未使用) */
    u32 esp2;           /* Ring2 ESP (未使用) */
    u32 ss2;            /* Ring2 SS  (未使用) */
    u32 cr3;            /* ページディレクトリ (タスク切替時、未使用) */
    u32 eip;            /* 未使用 (ソフトウェアタスク切替) */
    u32 eflags;         /* 未使用 */
    u32 eax;            /* 未使用 */
    u32 ecx;            /* 未使用 */
    u32 edx;            /* 未使用 */
    u32 ebx;            /* 未使用 */
    u32 esp;            /* 未使用 */
    u32 ebp;            /* 未使用 */
    u32 esi;            /* 未使用 */
    u32 edi;            /* 未使用 */
    u32 es;             /* 未使用 */
    u32 cs;             /* 未使用 */
    u32 ss;             /* 未使用 */
    u32 ds;             /* 未使用 */
    u32 fs;             /* 未使用 */
    u32 gs;             /* 未使用 */
    u32 ldt;            /* LDTセレクタ (未使用) */
    u16 trap;           /* トラップフラグ (0) */
    u16 iomap_base;     /* I/Oビットマップのオフセット (104 = sizeof(tss_entry前半)) */
    u8  iomap[TSS_IOMAP_SIZE + 1]; /* I/Oビットマップ + 終端バイト (0xFF) */
} __attribute__((packed));

/* TSS初期化 (esp0 = カーネルスタックトップ) */
void tss_init(u32 kernel_esp0);

/* TSSインスタンス (gdt.cから参照される) */
extern struct tss_entry kernel_tss;

/* TSS の ESP0 を更新 (V86タスク切替時等) */
void tss_set_esp0(u32 esp0);

/* TSS の現在の ESP0 を取得 (V86開始前に保存するため) */
u32 tss_get_esp0(void);

/* I/Oビットマップのポート許可/拒否 */
void tss_iomap_allow(u16 port);     /* ビット=0: 直接アクセス許可 */
void tss_iomap_deny(u16 port);      /* ビット=1: トラップ (#GP) */
void tss_iomap_allow_range(u16 start, u16 end);
void tss_iomap_deny_all(void);      /* 全ポートをトラップに設定 */

#endif /* TSS_H */
