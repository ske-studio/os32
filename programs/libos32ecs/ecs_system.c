/* ======================================================================== */
/*  ECS_SYSTEM.C — 組み込み System 実装                                    */
/*                                                                          */
/*  sys_physics, sys_anim, sys_timer, sys_health, sys_chem_sync             */
/* ======================================================================== */

#include "libos32ecs.h"

/* ====================================================================== */
/*  外部参照                                                               */
/* ====================================================================== */

extern EcsEntity  *ecs__get_entities(void);
extern ecs_timer_cb ecs__get_timer_cb(void);

/* ====================================================================== */
/*  sys_physics — Velocity → Transform 適用                               */
/* ====================================================================== */

void sys_physics(void)
{
    int i;
    u32 mask = COMP_TRANSFORM | COMP_VELOCITY;
    EcsEntity *ents = ecs__get_entities();

    for (i = 0; i < ECS_MAX_ENTITIES; i++) {
        CompTransform *t;
        CompVelocity  *v;

        if (ents[i].alive != ECS_ALIVE) continue;
        if ((ents[i].comp_mask & mask) != mask) continue;

        t = ecs_get_transform((ecs_entity_t)i);
        v = ecs_get_velocity((ecs_entity_t)i);

        /* 加速度 → 速度 */
        v->vx += v->ax;
        v->vy += v->ay;

        /* 速度 → 座標 */
        t->x += v->vx;
        t->y += v->vy;

        /* タイル座標同期 (整数部を抽出) */
        t->tile_x = (i16)(t->x >> 16);
        t->tile_y = (i16)(t->y >> 16);
    }
}

/* ====================================================================== */
/*  sys_anim — アニメーションフレーム進行                                  */
/* ====================================================================== */

void sys_anim(void)
{
    int i;
    u32 mask = COMP_SPRITE | COMP_ANIM;
    EcsEntity *ents = ecs__get_entities();

    for (i = 0; i < ECS_MAX_ENTITIES; i++) {
        CompSprite *spr;
        CompAnim   *anim;

        if (ents[i].alive != ECS_ALIVE) continue;
        if ((ents[i].comp_mask & mask) != mask) continue;

        anim = ecs_get_anim((ecs_entity_t)i);
        if (anim->done) continue;
        if (anim->speed == 0) continue;

        anim->counter++;
        if (anim->counter >= anim->speed) {
            anim->counter = 0;
            anim->frame_idx++;

            /* フレーム更新をスプライトに反映 */
            spr = ecs_get_sprite((ecs_entity_t)i);
            spr->frame = anim->frame_idx;

            /* ループ判定は外部のアニメーション定義に依存するため、
               ここでは frame_idx の巻き戻しは行わない。
               ゲーム側の System で anim_id に応じた max_frame を参照して
               frame_idx をリセットするか done=1 にする。 */
        }
    }
}

/* ====================================================================== */
/*  sys_timer — タイマー減算・コールバック                                  */
/* ====================================================================== */

void sys_timer(void)
{
    int i;
    u32 mask = COMP_TIMER;
    EcsEntity *ents = ecs__get_entities();
    ecs_timer_cb cb = ecs__get_timer_cb();

    for (i = 0; i < ECS_MAX_ENTITIES; i++) {
        CompTimer *tmr;

        if (ents[i].alive != ECS_ALIVE) continue;
        if ((ents[i].comp_mask & mask) != mask) continue;

        tmr = ecs_get_timer((ecs_entity_t)i);
        if (tmr->remaining == 0) continue;

        tmr->remaining--;
        if (tmr->remaining == 0) {
            /* 満了コールバック */
            if (cb) {
                cb((ecs_entity_t)i, tmr->callback_id);
            }
            /* 自動リピート */
            if (tmr->auto_repeat) {
                tmr->remaining = tmr->duration;
            }
        }
    }
}

/* ====================================================================== */
/*  sys_health — HP=0 で破棄予約                                          */
/* ====================================================================== */

void sys_health(void)
{
    int i;
    u32 mask = COMP_HEALTH;
    EcsEntity *ents = ecs__get_entities();

    for (i = 0; i < ECS_MAX_ENTITIES; i++) {
        CompHealth *hp;

        if (ents[i].alive != ECS_ALIVE) continue;
        if ((ents[i].comp_mask & mask) != mask) continue;

        hp = ecs_get_health((ecs_entity_t)i);

        /* 無敵時間の減算 */
        if (hp->invincible > 0) {
            hp->invincible--;
        }

        /* HP が 0 以下なら破棄予約 */
        if (hp->hp <= 0) {
            ecs_destroy((ecs_entity_t)i);
        }
    }
}

/* ====================================================================== */
/*  sys_chem_sync — ChemObject ↔ Entity 座標同期 (スタブ)                 */
/*                                                                          */
/*  libos32chem との実際の連携は Phase 3 で実装する。                       */
/*  ここではフレームワークのみ。                                            */
/* ====================================================================== */

void sys_chem_sync(void)
{
    /* Phase 3: libos32chem 連携時に実装
     *
     * 処理内容:
     *   1. COMP_TRANSFORM | COMP_CHEM を持つ Entity を走査
     *   2. chem_id が有効なら、chem_get() で ChemObject を取得
     *   3. ECS側の座標を ChemObject に反映 (ECSが主)
     *   4. ChemObject の状態変化 (temperature, hp) を ECS側に反映
     */
}
