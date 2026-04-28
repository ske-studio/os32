/* ======================================================================== */
/*  ECS_DEMO.C — libos32ecs CUI デモ                                       */
/*                                                                          */
/*  Entity の生成・物理移動・タイマー・HP減少・破棄・デバッグダンプを         */
/*  CUIコンソール上で実演する。libos32chem との連携テスト含む。              */
/* ======================================================================== */

#include "os32api.h"
#include "libos32ecs.h"
#include "libos32chem.h"

extern KernelAPI *kapi;
#define api kapi

/* 内部アクセサ (ecs_core.c) */
extern EcsEntity *ecs__get_entities(void);

/* タグ定数 */
#define TAG_PLAYER    1
#define TAG_ENEMY     2
#define TAG_PROJECTILE 3

/* ---- chem アダプタ ---- */
static void chem_adapter(ecs_entity_t e, i16 chem_id, CompTransform *t)
{
    const ChemObject *co = chem_get(chem_id);
    if (!co) {
        /* ChemObject が消滅していたら chem_id をクリア */
        ecs_get_chem(e)->chem_id = -1;
        return;
    }

    /* ECS → Chem: ECS側の座標を主として同期 */
    /* (chem_spawn 時に設定済みなので、移動した場合のみ反映) */
    /* ここでは簡易版: Chem → ECS の状態反映のみ */

    /* Chem → ECS: HP同期 (化学反応でHPが減った場合) */
    if (ecs_has(e, COMP_HEALTH)) {
        CompHealth *hp = ecs_get_health(e);
        if (co->hp < hp->hp) {
            hp->hp = co->hp;
        }
    }
}

/* ---- ライフサイクルコールバック ---- */
static void on_destroy(ecs_entity_t e)
{
    CompChem *cc;
    /* COMP_CHEM を持つ Entity が破棄されたら、ChemObject も一緒に消す */
    if (!ecs_has(e, COMP_CHEM)) return;
    cc = ecs_get_chem(e);
    if (cc->chem_id >= 0) {
        chem_destroy(cc->chem_id);
        cc->chem_id = -1;
    }
}

/* ---- Entity 生成ヘルパー ---- */
static ecs_entity_t spawn_player(i16 x, i16 y)
{
    ecs_entity_t e = ecs_create();
    CompTransform *t;
    CompHealth *hp;
    CompTag *tag;

    if (e == ECS_INVALID) return ECS_INVALID;

    ecs_add(e, COMP_TRANSFORM | COMP_VELOCITY | COMP_HEALTH | COMP_TAG);

    t = ecs_get_transform(e);
    t->x = FIX16_FROM_INT(x);
    t->y = FIX16_FROM_INT(y);
    t->tile_x = x;
    t->tile_y = y;

    hp = ecs_get_health(e);
    hp->hp = 100;
    hp->max_hp = 100;

    tag = ecs_get_tag(e);
    tag->tag = TAG_PLAYER;

    return e;
}

static ecs_entity_t spawn_enemy(i16 x, i16 y, i16 hp_val)
{
    ecs_entity_t e = ecs_create();
    CompTransform *t;
    CompVelocity *v;
    CompHealth *hp;
    CompTag *tag;

    if (e == ECS_INVALID) return ECS_INVALID;

    ecs_add(e, COMP_TRANSFORM | COMP_VELOCITY | COMP_HEALTH |
               COMP_TAG | COMP_TIMER);

    t = ecs_get_transform(e);
    t->x = FIX16_FROM_INT(x);
    t->y = FIX16_FROM_INT(y);
    t->tile_x = x;
    t->tile_y = y;

    /* 右方向に移動 */
    v = ecs_get_velocity(e);
    v->vx = FIX16_FROM_INT(1);
    v->vy = 0;

    hp = ecs_get_health(e);
    hp->hp = hp_val;
    hp->max_hp = hp_val;

    tag = ecs_get_tag(e);
    tag->tag = TAG_ENEMY;

    return e;
}

static ecs_entity_t spawn_projectile(i16 x, i16 y, fix16_t vx, fix16_t vy)
{
    ecs_entity_t e = ecs_create();
    CompTransform *t;
    CompVelocity *v;
    CompTimer *tmr;
    CompTag *tag;

    if (e == ECS_INVALID) return ECS_INVALID;

    ecs_add(e, COMP_TRANSFORM | COMP_VELOCITY | COMP_TIMER | COMP_TAG);

    t = ecs_get_transform(e);
    t->x = FIX16_FROM_INT(x);
    t->y = FIX16_FROM_INT(y);
    t->tile_x = x;
    t->tile_y = y;

    v = ecs_get_velocity(e);
    v->vx = vx;
    v->vy = vy;

    /* 30フレーム後に消滅 */
    tmr = ecs_get_timer(e);
    tmr->remaining = 30;
    tmr->duration = 30;
    tmr->auto_repeat = 0;
    tmr->callback_id = 1;  /* TIMER_EXPIRE_DESTROY */

    tag = ecs_get_tag(e);
    tag->tag = TAG_PROJECTILE;

    return e;
}

/* タイマー満了 → Entity 破棄 */
static void on_timer_expire(ecs_entity_t e, u8 cb_id)
{
    if (cb_id == 1) {
        ecs_destroy(e);
    }
}

/* ---- ゲーム固有 System: 衝突判定 ---- */
static void sys_simple_collision(void)
{
    int i, j;
    EcsEntity *ents = ecs__get_entities();
    u32 proj_mask = COMP_TRANSFORM | COMP_TAG;
    u32 enemy_mask = COMP_TRANSFORM | COMP_HEALTH | COMP_TAG;

    for (i = 0; i < ECS_MAX_ENTITIES; i++) {
        CompTransform *pt;
        CompTag *ptag;

        if (ents[i].alive != ECS_ALIVE) continue;
        if ((ents[i].comp_mask & proj_mask) != proj_mask) continue;
        ptag = ecs_get_tag((ecs_entity_t)i);
        if (ptag->tag != TAG_PROJECTILE) continue;

        pt = ecs_get_transform((ecs_entity_t)i);

        for (j = 0; j < ECS_MAX_ENTITIES; j++) {
            CompTransform *et;
            CompTag *etag;
            CompHealth *ehp;

            if (i == j) continue;
            if (ents[j].alive != ECS_ALIVE) continue;
            if ((ents[j].comp_mask & enemy_mask) != enemy_mask) continue;
            etag = ecs_get_tag((ecs_entity_t)j);
            if (etag->tag != TAG_ENEMY) continue;

            et = ecs_get_transform((ecs_entity_t)j);

            /* 同一タイルで衝突判定 */
            if (pt->tile_x == et->tile_x && pt->tile_y == et->tile_y) {
                ehp = ecs_get_health((ecs_entity_t)j);
                ehp->hp -= 25;
                ecs_destroy((ecs_entity_t)i); /* 弾消滅 */
                api->kprintf(ATTR_YELLOW,
                    "  HIT! proj[%d] -> enemy[%d] hp=%d\n",
                    i, j, (int)ehp->hp);
                break;
            }
        }
    }
}

/* ---- フレーム表示 ---- */
static void print_frame(int frame)
{
    int i;
    EcsEntity *ents = ecs__get_entities();

    api->kprintf(ATTR_CYAN, "\n--- Frame %d (active=%d) ---\n",
                 frame, ecs_active_count());

    for (i = 0; i < ECS_MAX_ENTITIES; i++) {
        CompTransform *t;
        CompTag *tag;
        const char *name;

        if (ents[i].alive != ECS_ALIVE) continue;
        if (!(ents[i].comp_mask & COMP_TRANSFORM)) continue;

        t = ecs_get_transform((ecs_entity_t)i);
        name = "???";
        if (ents[i].comp_mask & COMP_TAG) {
            tag = ecs_get_tag((ecs_entity_t)i);
            if (tag->tag == TAG_PLAYER) name = "PLAYER";
            else if (tag->tag == TAG_ENEMY) name = "ENEMY";
            else if (tag->tag == TAG_PROJECTILE) name = "PROJ";
        }

        api->kprintf(ATTR_WHITE, "  [%d] %s (%d,%d)",
                     i, name, (int)t->tile_x, (int)t->tile_y);

        if (ents[i].comp_mask & COMP_HEALTH) {
            CompHealth *hp = ecs_get_health((ecs_entity_t)i);
            api->kprintf(ATTR_WHITE, " hp=%d/%d",
                         (int)hp->hp, (int)hp->max_hp);
        }
        if (ents[i].comp_mask & COMP_TIMER) {
            CompTimer *tmr = ecs_get_timer((ecs_entity_t)i);
            api->kprintf(ATTR_WHITE, " tmr=%d",
                         (int)tmr->remaining);
        }
        api->kprintf(ATTR_WHITE, "\n");
    }
}

/* ====================================================================== */
/*  メインループ                                                           */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    int frame;
    ecs_entity_t player, enemy1, enemy2;

    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "=== libos32ecs demo ===\n\n");

    /* ECS 初期化 */
    ecs_init();

    /* コールバック設定 */
    ecs_set_timer_callback(on_timer_expire);
    ecs_on_destroy(on_destroy);
    ecs_set_chem_adapter(chem_adapter);

    /* System 登録 (優先度順) */
    ecs_register_system(sys_physics, 10,
                        COMP_TRANSFORM | COMP_VELOCITY);
    ecs_register_system(sys_simple_collision, 30,
                        COMP_TRANSFORM | COMP_TAG);
    ecs_register_system(sys_chem_sync, 40,
                        COMP_TRANSFORM | COMP_CHEM);
    ecs_register_system(sys_timer, 80, COMP_TIMER);
    ecs_register_system(sys_health, 90, COMP_HEALTH);

    /* Entity 生成 */
    api->kprintf(ATTR_WHITE, "Spawning entities...\n");
    player = spawn_player(5, 5);
    enemy1 = spawn_enemy(10, 5, 50);  /* 右方向に移動する敵 */
    enemy2 = spawn_enemy(15, 5, 30);  /* HP低い敵 */

    /* Frame 0: 弾を発射 */
    api->kprintf(ATTR_WHITE, "Player fires projectile!\n");
    spawn_projectile(6, 5, FIX16_FROM_INT(2), 0); /* 右方向に2px/f */

    print_frame(0);

    /* シミュレーション: 10フレーム */
    for (frame = 1; frame <= 10; frame++) {
        ecs_update();
        ecs_cleanup();
        print_frame(frame);
    }

    /* 敵2のHPを直接0にして Health System に破棄させる */
    if (ecs_alive(enemy2)) {
        CompHealth *hp2 = ecs_get_health(enemy2);
        hp2->hp = 0;
        api->kprintf(ATTR_RED, "\nForcing enemy2 hp=0\n");
        ecs_update();
        ecs_cleanup();
        print_frame(11);
    }

    /* デバッグダンプ */
    api->kprintf(ATTR_WHITE, "\n");
    ecs_debug_dump();

    /* クエリテスト: プレイヤーをタグで検索 */
    {
        ecs_entity_t found = ecs_find_by_tag(TAG_PLAYER);
        if (found != ECS_INVALID) {
            CompTransform *t = ecs_get_transform(found);
            api->kprintf(ATTR_GREEN,
                "\nPlayer found at (%d,%d)\n",
                (int)t->tile_x, (int)t->tile_y);
        }
    }

    /* 範囲検索: (0,0)-(20,10) */
    {
        ecs_entity_t buf[16];
        int n = ecs_find_in_rect(0, 0, 20, 10, buf, 16);
        api->kprintf(ATTR_GREEN,
            "Entities in (0,0)-(20,10): %d\n", n);
    }

    /* 後始末 */
    ecs_shutdown();
    api->kprintf(ATTR_CYAN, "\n=== demo complete ===\n");

    (void)player;
    (void)enemy1;
    return 0;
}
