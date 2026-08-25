/* ======================================================================== */
/*  CHEM_REACT.C — 化学エンジン 反応処理                                    */
/*                                                                          */
/*  2オブジェクト間の衝突時に反応ルールを検索・適用する。                     */
/*  ルールはRAMキャッシュ上の配列を線形探索 (O(n), n<=64)。                  */
/*  ゲーム側が衝突検出を行い、接触ペアをこの関数に渡す想定。                 */
/*                                                                          */
/*  SPREAD アクション: 属性を周囲のオブジェクトに伝播する。                  */
/*  再帰深度は CHEM_SPREAD_DEPTH (3) で制限される。                          */
/* ======================================================================== */

#include "libos32chem.h"
#include "libos32math.h"   /* fast_distance_sq */

/* crt0_c.c で定義される KernelAPI ポインタ */
extern KernelAPI *kapi;
#define api kapi

/* chem_core.c の内部グローバルへのアクセサ */
extern ChemObject    *chem__get_objects(void);
extern ChemReaction  *chem__get_reactions(void);
extern int            chem__get_reaction_count(void);
extern chem_reaction_callback chem__get_callback(void);

/* SPREAD時の伝播半径 (タイル座標単位) */
#define SPREAD_RADIUS  3

/* ====================================================================== */
/*  アクション適用                                                         */
/* ====================================================================== */

/* アクションを単一オブジェクトに適用する */
static void apply_action_to(ChemObject *obj, const ChemReaction *rule)
{
    i32 t;

    switch (rule->action) {
    case CHEM_ACT_IGNITE:
        obj->state = CHEM_STATE_BURNING;
        obj->elements |= ELEM_FIRE;
        break;

    case CHEM_ACT_EXTINGUISH:
        obj->state = CHEM_STATE_IDLE;
        obj->elements &= ~ELEM_FIRE;
        break;

    case CHEM_ACT_FREEZE:
        obj->state = CHEM_STATE_FROZEN;
        obj->elements |= ELEM_ICE;
        obj->elements &= ~ELEM_WATER;
        break;

    case CHEM_ACT_MELT:
        obj->state = CHEM_STATE_IDLE;
        obj->elements &= ~ELEM_ICE;
        obj->elements |= ELEM_WATER;
        break;

    case CHEM_ACT_EVAPORATE:
        obj->state = CHEM_STATE_IDLE;
        obj->elements &= ~ELEM_WATER;
        obj->elements |= ELEM_STEAM;
        break;

    case CHEM_ACT_ELECTRIFY:
        obj->state = CHEM_STATE_CHARGED;
        obj->elements |= ELEM_ELECTRIC;
        break;

    case CHEM_ACT_DAMAGE:
        /* hp_delta は負の値でダメージ */
        t = (i32)obj->hp + (i32)rule->hp_delta;
        if (t < 0) t = 0;
        if (t > 32767) t = 32767;
        obj->hp = (i16)t;
        break;

    case CHEM_ACT_DESTROY:
        obj->hp = 0;
        break;

    case CHEM_ACT_SPAWN:
        /* SPAWNはオブジェクト生成のため、ここでは属性追加のみ */
        /* 実際のオブジェクト生成は chem_react 側で行う */
        break;

    case CHEM_ACT_SPREAD:
        /* SPREADは chem_react 側で処理 */
        break;

    default:
        break;
    }

    /* 温度変化 */
    if (rule->temp_delta != 0) {
        t = (i32)obj->temperature + (i32)rule->temp_delta;
        if (t > 32767) t = 32767;
        if (t < -32768) t = -32768;
        obj->temperature = (i16)t;
    }
}

/* ====================================================================== */
/*  SPREAD: 属性伝播 (再帰深度制限付き)                                    */
/* ====================================================================== */

/* 現在の再帰深度 (リエントラント防止) */
static int g_spread_depth;

/*
 * 指定オブジェクトの属性を周囲に伝播する。
 *   source_idx: 伝播元のオブジェクト配列インデックス
 *   elem:       伝播する属性ビットマスク
 *   rule:       元となった反応ルール (temp_delta等を適用)
 *
 * 周囲 SPREAD_RADIUS 以内のオブジェクトで、
 * まだ該当属性を持っていないものに対して属性を付与し、
 * 再帰的にさらに伝播を続ける (深度制限あり)。
 */
static void spread_to_nearby(int source_idx, u32 elem,
                             const ChemReaction *rule)
{
    ChemObject *objects;
    int i;
    u32 rsq;
    i16 sx, sy;

    if (g_spread_depth >= CHEM_SPREAD_DEPTH) return;
    g_spread_depth++;

    objects = chem__get_objects();
    sx = objects[source_idx].x;
    sy = objects[source_idx].y;
    rsq = (u32)(SPREAD_RADIUS * SPREAD_RADIUS);

    for (i = 0; i < CHEM_MAX_OBJECTS; i++) {
        u32 dist_sq;
        ChemObject *obj;
        i32 t;

        if (i == source_idx) continue;
        if (objects[i].id == 0) continue;

        obj = &objects[i];

        /* 既にこの属性を持っている場合はスキップ (重複伝播防止) */
        if ((obj->elements & elem) == elem) continue;

        dist_sq = fast_distance_sq(
            (int)(obj->x - sx),
            (int)(obj->y - sy));
        if (dist_sq > rsq) continue;

        /* 属性を付与 */
        obj->elements |= elem;

        /* 温度変化を適用 (減衰: 深さに応じて半減) */
        if (rule->temp_delta != 0) {
            i16 delta;
            delta = (i16)(rule->temp_delta >> g_spread_depth);
            t = (i32)obj->temperature + (i32)delta;
            if (t > 32767) t = 32767;
            if (t < -32768) t = -32768;
            obj->temperature = (i16)t;
        }

        /* 火なら着火 */
        if (elem & ELEM_FIRE) {
            obj->state = CHEM_STATE_BURNING;
        }
        /* 電気なら帯電 */
        if (elem & ELEM_ELECTRIC) {
            obj->state = CHEM_STATE_CHARGED;
        }

        /* コールバック発火 */
        {
            chem_reaction_callback cb = chem__get_callback();
            if (cb) {
                cb(source_idx, i, CHEM_ACT_SPREAD, elem);
            }
        }

        /* 再帰伝播 */
        spread_to_nearby(i, elem, rule);
    }

    g_spread_depth--;
}

/* ====================================================================== */
/*  公開API: chem_react                                                    */
/* ====================================================================== */

int chem_react(int obj_a, int obj_b)
{
    ChemObject *objects;
    ChemReaction *reactions;
    ChemObject *a;
    ChemObject *b;
    int reaction_count;
    int applied;
    int i;
    chem_reaction_callback cb;

    objects = chem__get_objects();
    reactions = chem__get_reactions();
    reaction_count = chem__get_reaction_count();
    cb = chem__get_callback();

    /* バリデーション */
    if (obj_a < 0 || obj_a >= CHEM_MAX_OBJECTS) return 0;
    if (obj_b < 0 || obj_b >= CHEM_MAX_OBJECTS) return 0;

    a = &objects[obj_a];
    b = &objects[obj_b];

    if (a->id == 0 || b->id == 0) return 0;

    applied = 0;

    /* ルールキャッシュを線形探索 (既に優先度降順でソート済み) */
    for (i = 0; i < reaction_count; i++) {
        ChemReaction *r = &reactions[i];

        /* 双方向マッチ: (a.elem & r.elem_a) && (b.elem & r.elem_b) */
        /*            OR (a.elem & r.elem_b) && (b.elem & r.elem_a) */
        int match_forward;
        int match_reverse;

        match_forward = ((a->elements & r->elem_a) == r->elem_a) &&
                        ((b->elements & r->elem_b) == r->elem_b);
        match_reverse = ((a->elements & r->elem_b) == r->elem_b) &&
                        ((b->elements & r->elem_a) == r->elem_a);

        if (!match_forward && !match_reverse) continue;

        /* ルール適用 */
        switch (r->target) {
        case CHEM_TGT_A:
            if (match_forward) {
                apply_action_to(a, r);
            } else {
                apply_action_to(b, r);
            }
            break;
        case CHEM_TGT_B:
            if (match_forward) {
                apply_action_to(b, r);
            } else {
                apply_action_to(a, r);
            }
            break;
        case CHEM_TGT_BOTH:
            apply_action_to(a, r);
            apply_action_to(b, r);
            break;
        case CHEM_TGT_AREA:
            apply_action_to(a, r);
            apply_action_to(b, r);
            /* AREA はゲーム側が chem_apply_area で別途処理 */
            break;
        }

        /* SPAWN アクション: 新オブジェクト生成 */
        if (r->action == CHEM_ACT_SPAWN && r->spawn_elem != 0) {
            /* 中間座標に生成 */
            i16 mx = (i16)(((i32)a->x + (i32)b->x) / 2);
            i16 my = (i16)(((i32)a->y + (i32)b->y) / 2);
            int new_id = chem_spawn(0, mx, my);
            if (new_id >= 0) {
                objects[new_id].elements = (u32)r->spawn_elem;
            }
        }

        /* SPREAD アクション: 属性を周囲に伝播 */
        if (r->action == CHEM_ACT_SPREAD) {
            /* elem_b側 (伝播される属性を持つ側) を起点に伝播 */
            int spread_src;
            u32 spread_elem;

            if (match_forward) {
                spread_src = obj_b;
                spread_elem = r->elem_b;
            } else {
                spread_src = obj_a;
                spread_elem = r->elem_a;
            }

            g_spread_depth = 0;
            spread_to_nearby(spread_src, spread_elem, r);
        }

        /* コールバック発火 */
        if (cb) {
            cb(obj_a, obj_b, r->action, (u32)r->spawn_elem);
        }

        applied++;
    }

    return applied;
}

