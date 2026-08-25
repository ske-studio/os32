/* ======================================================================== */
/*  CHEM_UPDATE.C — 化学エンジン 毎フレーム更新                             */
/*                                                                          */
/*  毎フレーム呼ばれるホットパス。SQL不使用、RAMキャッシュのみ参照。          */
/*  - 燃焼中オブジェクトの温度上昇                                           */
/*  - タイマーデクリメント                                                   */
/*  - HP <= 0 のオブジェクト消滅                                             */
/*  - 温度ベース状態遷移チェック                                             */
/* ======================================================================== */

#include "libos32chem.h"

/* chem_core.c の内部グローバルへのアクセサ */
extern ChemObject   *chem__get_objects(void);
extern ChemPhaseRule *chem__get_phases(void);
extern int            chem__get_phase_count(void);

extern void *memset(void *, int, unsigned int);

/* 燃焼時の温度上昇速度 (フレームあたり) */
#define BURN_TEMP_RATE  2

/* ====================================================================== */
/*  内部: 状態遷移チェック                                                 */
/* ====================================================================== */

static void check_phase_transition(ChemObject *obj)
{
    ChemPhaseRule *phases;
    int phase_count;
    int i;

    phases = chem__get_phases();
    phase_count = chem__get_phase_count();

    for (i = 0; i < phase_count; i++) {
        ChemPhaseRule *p = &phases[i];

        /* 現在のオブジェクトが遷移元属性を持っているか */
        if ((obj->elements & p->elem_from) != p->elem_from) continue;

        /* 温度が範囲内か */
        if (obj->temperature < p->temp_min) continue;
        if (obj->temperature > p->temp_max) continue;

        /* 遷移を適用: 元属性を除去して変化先属性を付与 */
        obj->elements &= ~p->elem_from;
        obj->elements |= p->elem_to;

        /* 副産物があれば属性追加 (実際のオブジェクト生成はゲーム側) */
        if (p->spawn_elem != 0) {
            obj->elements |= (u32)p->spawn_elem;
        }

        /* 状態を遷移先に合わせて更新 */
        if (p->elem_to & ELEM_ICE) {
            obj->state = CHEM_STATE_FROZEN;
        } else if (p->elem_to & ELEM_WATER) {
            obj->state = CHEM_STATE_WET;
        } else if (p->elem_to & ELEM_STEAM) {
            obj->state = CHEM_STATE_IDLE;
        } else {
            obj->state = CHEM_STATE_IDLE;
        }

        /* 一度に一つの遷移のみ適用 */
        break;
    }
}

/* ====================================================================== */
/*  公開API: chem_update                                                   */
/* ====================================================================== */

void chem_update(void)
{
    ChemObject *objects;
    int i;
    i32 t;

    objects = chem__get_objects();

    for (i = 0; i < CHEM_MAX_OBJECTS; i++) {
        ChemObject *obj = &objects[i];

        /* 未使用スロットはスキップ */
        if (obj->id == 0) continue;

        /* --- 燃焼中: 温度上昇 --- */
        if (obj->state == CHEM_STATE_BURNING) {
            t = (i32)obj->temperature + BURN_TEMP_RATE;
            if (t > 32767) t = 32767;
            obj->temperature = (i16)t;
        }

        /* --- タイマーデクリメント --- */
        if (obj->timer > 0) {
            obj->timer--;
        }

        /* --- HP <= 0: 消滅処理 --- */
        if (obj->hp <= 0) {
            memset(obj, 0, sizeof(ChemObject));
            continue;
        }

        /* --- 温度ベース状態遷移チェック --- */
        check_phase_transition(obj);
    }
}
