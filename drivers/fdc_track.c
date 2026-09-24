/* ======================================================================== */
/*  FDC_TRACK.C — FD の読み出しをトラック単位に束ねる (I/O を触らない)      */
/*                                                                          */
/*  ここには I/O もタイマも置かない。読むのは ops 経由だけ。                */
/*  理由と契約は fdc_track.h、試験は tools/tests/test_fdc_track.py。        */
/* ======================================================================== */

#include "fdc_track.h"

/* ======================================================================== */
/*  要求を 1 トラックの中に切る                                             */
/* ======================================================================== */
int fdc_track_split(const struct fdc_geom *g, u32 lba, u32 count,
                    struct fdc_run *run)
{
    u32 spt, heads, per_cyl, in_track, left;

    if (g == 0 || count == 0 || g->spt == 0 || g->heads == 0) return 0;

    spt = (u32)g->spt;
    heads = (u32)g->heads;
    per_cyl = spt * heads;

    run->cyl  = (int)(lba / per_cyl);
    run->head = (int)((lba / spt) % heads);
    run->sect = (int)(lba % spt) + 1;

    /* トラックの残り (sect..spt) と要求の残りの小さい方。
     * **ヘッドの境目でも切る** — MT (マルチトラック) は使わない
     * (理由は fdc.c の fdc_read_sectors の注記)。 */
    left = spt - (lba % spt);
    in_track = (count < left) ? count : left;
    run->count = (int)in_track;
    return run->count;
}

/* ======================================================================== */
/*  1 回で何番のセクタまで読むか                                            */
/* ======================================================================== */
int fdc_track_eot(const struct fdc_geom *g, const struct fdc_run *run)
{
    int last = run->sect + run->count - 1;

    if (run->sect < 1 || run->count < 1 || last > (int)g->spt) return 0;
#if FDC_TRACK_READ_AHEAD
    return (int)g->spt;
#else
    return last;
#endif
}

/* ======================================================================== */
/*  中身の出し入れ                                                          */
/* ======================================================================== */
void fdc_track_init(struct fdc_track_cache *c, int i, u8 *buf)
{
    if (i < 0 || i >= FDC_TRACK_SLOTS) return;
    c->slot[i].valid = 0;
    c->slot[i].buf = buf;
}

void fdc_track_invalidate(struct fdc_track_cache *c)
{
    int i;
    for (i = 0; i < FDC_TRACK_SLOTS; i++) c->slot[i].valid = 0;
    c->bad_valid = 0;
}

/* 先読みが失敗した印の付いたトラックか。 */
static int fdc_track_is_bad(const struct fdc_track_cache *c, int drv,
                            const struct fdc_geom *g, const struct fdc_run *run)
{
    return c->bad_valid && c->bad_geom == g && c->bad_drv == drv
        && c->bad_cyl == run->cyl && c->bad_head == run->head;
}

int fdc_track_hit(const struct fdc_track_cache *c, int drv,
                  const struct fdc_geom *g, u32 gen,
                  const struct fdc_run *run)
{
    int i;

    for (i = 0; i < FDC_TRACK_SLOTS; i++) {
        const struct fdc_track_slot *t = &c->slot[i];
        if (!t->valid || t->buf == 0) continue;
        /* 書き込み・Ready 変化・メディアの変更で世代が進んだら別物。 */
        if (t->gen != gen) continue;
        /* ジオメトリが変わった (1.44MB ⇔ 2HD の差し替え) なら別物。 */
        if (t->geom != g) continue;
        if (t->drv != drv || t->cyl != run->cyl || t->head != run->head) continue;
        if (run->sect < t->first) continue;
        if (run->sect + run->count - 1 > t->last) continue;
        return i;
    }
    return -1;
}

/* 埋める先のスロット: 空き、無ければいちばん長く使っていないもの。 */
static int fdc_track_victim(const struct fdc_track_cache *c)
{
    int i, v = -1;

    for (i = 0; i < FDC_TRACK_SLOTS; i++) {
        if (c->slot[i].buf == 0) continue;
        if (!c->slot[i].valid) return i;
        if (v < 0 || c->slot[i].used < c->slot[v].used) v = i;
    }
    return v;
}

/* 受け皿 (FDC_TRACK_MAX_BYTES) に 1 トラックが入るか。 */
static int fdc_track_fits(const struct fdc_geom *g)
{
    return (u32)g->spt * (u32)g->bps <= (u32)FDC_TRACK_MAX_BYTES;
}

/* 区間を 1 セクタずつ buff へ読む (旧来の読み方。束ねた読みの受け身)。 */
static int fdc_track_read_singly(const struct fdc_track_ops *ops, int drv,
                                 const struct fdc_geom *g,
                                 const struct fdc_run *run, u8 *dst)
{
    int i;

    for (i = 0; i < run->count; i++) {
        if (ops->read_one(ops->ctx, drv, run->cyl, run->head, run->sect + i,
                          g, dst + (u32)i * g->bps) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ======================================================================== */
/*  読み出し本体                                                            */
/* ======================================================================== */
int fdc_track_read(struct fdc_track_cache *c, const struct fdc_track_ops *ops,
                   int drv, const struct fdc_geom *g,
                   u32 lba, u32 count, u8 *buff)
{
    struct fdc_run run;
    u8 *dst = buff;

    while (count > 0) {
        int n = fdc_track_split(g, lba, count, &run);
        int hit;
        u32 bytes, gen;

        if (n <= 0) return -1;
        bytes = (u32)n * g->bps;
        /* 世代はこの区間の直前に聞く (前の区間の読みの途中で Ready 変化を
         * 見ていれば、ここで進んでいる)。 */
        gen = ops->gen(ops->ctx, drv);
        hit = fdc_track_hit(c, drv, g, gen, &run);

        if (hit >= 0) {
            /* 持っている中身から写す。 */
            struct fdc_track_slot *t = &c->slot[hit];
            ops->copy(dst, t->buf + (u32)(run.sect - t->first) * g->bps,
                      bytes);
            t->used = ++c->clock;
        } else if (!fdc_track_fits(g) || fdc_track_victim(c) < 0) {
            /* 受け皿に入らないジオメトリ / 受け皿が無い。旧来どおり読む。 */
            if (fdc_track_read_singly(ops, drv, g, &run, dst) != 0) return -1;
        } else {
            int eot = fdc_track_eot(g, &run);
            int want_last = run.sect + run.count - 1;
            struct fdc_track_slot *t = &c->slot[fdc_track_victim(c)];

            /* 先読みが落ちたトラックでは要求の範囲だけを読む。 */
            if (eot > want_last && fdc_track_is_bad(c, drv, g, &run)) {
                eot = want_last;
            }

            /* 先に捨てる — read_multi が途中まで t->buf を書き換えて
             * 失敗しても、半端な中身を当てない (印は残す)。 */
            t->valid = 0;
            if (eot > 0 &&
                ops->read_multi(ops->ctx, drv, run.cyl, run.head, run.sect,
                                eot - run.sect + 1, g, t->buf) == 0) {
                t->drv = drv;
                t->cyl = run.cyl;
                t->head = run.head;
                t->first = run.sect;
                t->last = eot;
                t->geom = g;
                /* **読む前に聞いた世代**で覚える。読みの途中で Ready 変化を
                 * 見て世代が進んでいれば、この中身は次から当たらない。 */
                t->gen = gen;
                t->used = ++c->clock;
                t->valid = 1;
                ops->copy(dst, t->buf, bytes);
            } else {
                /* 先読みの分まで読もうとして落ちたなら、このトラックに印を
                 * 付ける (次からは要求の範囲だけ)。 */
                if (eot > want_last) {
                    c->bad_valid = 1;
                    c->bad_drv = drv;
                    c->bad_cyl = run.cyl;
                    c->bad_head = run.head;
                    c->bad_geom = g;
                }
                /* まとめ読みが失敗した。**要求した区間だけ**を 1 セクタずつ
                 * 読み直す (リトライと回復は read_one = fdc 側が持つ)。
                 * 先読みの分まで読み直すと、傷んだセクタが要求の外に
                 * あるだけで読めるファイルまで失敗にしてしまう。 */
                if (fdc_track_read_singly(ops, drv, g, &run, dst) != 0) {
                    return -1;
                }
            }
        }

        dst += bytes;
        lba += (u32)n;
        count -= (u32)n;
    }
    return 0;
}
