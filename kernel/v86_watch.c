/* ======================================================================== */
/*  V86_WATCH.C - V86 メモリウォッチポイント (T2.4)                         */
/*                                                                          */
/*  PTE NOT_PRESENT 方式で V86 メモリアクセスを監視する。                   */
/*                                                                          */
/*  動作シーケンス:                                                         */
/*  1. ウォッチ設定時: 該当4KBページを NOT_PRESENT に設定                   */
/*  2. ゲストアクセス → #PF 発生                                           */
/*  3. #PF ハンドラ: ウォッチテーブルチェック → マッチなら記録              */
/*     → PTE を PRESENT に一時復帰 → EFLAGS.TF=1 セット                   */
/*  4. 1命令再実行 → #DB 発火                                              */
/*  5. #DB ハンドラ: PTE を再度 NOT_PRESENT に → TF クリア → V86復帰      */
/* ======================================================================== */

#include "v86_watch.h"
#include "v86.h"
#include "v86_mem.h"
#include "paging.h"
#include "kprintf.h"

/* tick_count */
extern volatile u32 tick_count;

/* ====================================================================== */
/*  ウォッチポイントテーブル                                                */
/* ====================================================================== */
struct v86_watch_entry {
    u32 addr;         /* 監視アドレス (リニア) */
    u32 size;         /* 監視サイズ (1-4) */
    u32 page_base;    /* アドレスが属する 4KB ページの先頭 */
    u32 saved_pte;    /* NOT_PRESENT にする前の PTE 値 */
    int active;       /* 1=有効 */
};

static struct v86_watch_entry watch_table[V86_WATCH_MAX];
static int watch_pending_restore = -1;  /* #DB で復帰するエントリ (-1=なし) */

/* ウォッチポイントヒットログ */
#define V86_WATCH_LOG_SIZE 64

static struct v86_watch_hit watch_log[V86_WATCH_LOG_SIZE];
static u32 watch_log_idx = 0;
static u32 watch_hit_count = 0;

/* ====================================================================== */
/*  内部ヘルパー: TLB フラッシュ                                            */
/* ====================================================================== */
static void flush_tlb_page(u32 addr)
{
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

/* ====================================================================== */
/*  v86_watch_set — ウォッチポイント設定                                    */
/* ====================================================================== */
void v86_watch_set(u32 addr, u32 size)
{
    int i;
    u32 page_base;

    if (size < 1) size = 1;
    if (size > 4) size = 4;

    /* 空きスロットを探す */
    for (i = 0; i < V86_WATCH_MAX; i++) {
        if (!watch_table[i].active) {
            page_base = addr & ~0xFFFU;

            watch_table[i].addr = addr;
            watch_table[i].size = size;
            watch_table[i].page_base = page_base;
            watch_table[i].active = 1;

            /* 該当ページを NOT_PRESENT に設定 */
            paging_set_not_present(page_base, page_base + 0x1000);
            flush_tlb_page(page_base);

            kprintf(0x0A, "[V86_WATCH] set: addr=%x size=%d page=%x\n",
                    (unsigned)addr, (int)size, (unsigned)page_base);
            return;
        }
    }
    kprintf(0x0E, "[V86_WATCH] no free slot (max=%d)\n", V86_WATCH_MAX);
}

/* ====================================================================== */
/*  v86_watch_clear — ウォッチポイント解除                                  */
/* ====================================================================== */
void v86_watch_clear(u32 addr)
{
    int i;
    for (i = 0; i < V86_WATCH_MAX; i++) {
        if (watch_table[i].active && watch_table[i].addr == addr) {
            u32 page_base = watch_table[i].page_base;

            /* PTE を PRESENT に復帰 (RW + User + Present) */
            paging_set_page(page_base, page_base, 0x07);
            flush_tlb_page(page_base);

            watch_table[i].active = 0;
            kprintf(0x0A, "[V86_WATCH] clear: addr=%x\n", (unsigned)addr);
            return;
        }
    }
}

/* ====================================================================== */
/*  v86_watch_clear_all — 全ウォッチポイント解除                            */
/* ====================================================================== */
void v86_watch_clear_all(void)
{
    int i;
    for (i = 0; i < V86_WATCH_MAX; i++) {
        if (watch_table[i].active) {
            u32 page_base = watch_table[i].page_base;
            paging_set_page(page_base, page_base, 0x07);
            flush_tlb_page(page_base);
            watch_table[i].active = 0;
        }
    }
    watch_pending_restore = -1;
}

/* ====================================================================== */
/*  v86_watch_check_pf — #PF ハンドラからの呼び出し                         */
/*                                                                          */
/*  fault_addr (CR2) がウォッチポイントに該当するか判定する。                */
/*  該当する場合:                                                           */
/*    1. ヒットを記録                                                       */
/*    2. PTE を一時的に PRESENT に復帰                                      */
/*    3. EFLAGS.TF=1 セット (次の #DB でPTEを戻す)                         */
/*  戻り値: 1=ウォッチポイント処理済み, 0=通常#PF                          */
/* ====================================================================== */
int v86_watch_check_pf(u32 fault_addr, u32 *regs)
{
    int i;
    u32 fault_page = fault_addr & ~0xFFFU;

    for (i = 0; i < V86_WATCH_MAX; i++) {
        if (!watch_table[i].active) continue;
        if (watch_table[i].page_base != fault_page) continue;

        /* ウォッチポイントヒット検出 */
        {
            struct v86_watch_hit *h;
            h = &watch_log[watch_log_idx % V86_WATCH_LOG_SIZE];
            h->tick = tick_count;
            h->addr = fault_addr;
            h->cs = (u16)regs[V86_REG_CS];
            h->ip = (u16)(regs[V86_REG_EIP] & 0xFFFF);
            h->value = 0;  /* PRESENT復帰後に読み取り可能 */
            watch_log_idx++;
            watch_hit_count++;
        }

        /* PTE を一時的に PRESENT に復帰 */
        paging_set_page(fault_page, fault_page, 0x07);
        flush_tlb_page(fault_page);

        /* EFLAGS.TF=1 セット — 1命令後に #DB が発火する */
        regs[V86_REG_EFLAGS] |= (1U << 8);

        /* 復帰待ちエントリを記録 */
        watch_pending_restore = i;

        return 1;  /* ウォッチポイント処理済み */
    }

    return 0;  /* 通常の #PF */
}

/* ====================================================================== */
/*  v86_watch_check_db — #DB ハンドラからの呼び出し                         */
/*                                                                          */
/*  ウォッチポイント復帰処理: TF シングルステップ後に                       */
/*  PTE を再度 NOT_PRESENT に設定し、TF をクリアする。                      */
/*  戻り値: 1=ウォッチポイント復帰処理済み, 0=通常#DB                      */
/* ====================================================================== */
int v86_watch_check_db(u32 *regs)
{
    int idx;

    if (watch_pending_restore < 0) return 0;

    idx = watch_pending_restore;
    watch_pending_restore = -1;

    if (idx < V86_WATCH_MAX && watch_table[idx].active) {
        u32 page_base = watch_table[idx].page_base;

        /* PTE を再度 NOT_PRESENT に */
        paging_set_not_present(page_base, page_base + 0x1000);
        flush_tlb_page(page_base);

        /* TF クリア */
        regs[V86_REG_EFLAGS] &= ~(1U << 8);

        return 1;  /* ウォッチポイント復帰処理済み */
    }

    return 0;
}

/* ====================================================================== */
/*  ログ取得 API                                                            */
/* ====================================================================== */
u32 v86_watch_get_hit_count(void)
{
    return watch_hit_count;
}

u32 v86_watch_get_log(struct v86_watch_hit *out, u32 max_entries)
{
    u32 count, i, start;

    count = watch_hit_count;
    if (count > V86_WATCH_LOG_SIZE) count = V86_WATCH_LOG_SIZE;
    if (count > max_entries) count = max_entries;

    start = (watch_hit_count > V86_WATCH_LOG_SIZE)
            ? (watch_log_idx % V86_WATCH_LOG_SIZE) : 0;

    for (i = 0; i < count; i++) {
        u32 idx = (start + i) % V86_WATCH_LOG_SIZE;
        out[i] = watch_log[idx];
    }

    return count;
}
