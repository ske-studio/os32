/* ======================================================================== */
/*  CHEM_QUERY.C — 化学エンジン クエリ関数                                  */
/*                                                                          */
/*  ゲーム側UIやロジック用のクエリAPI。                                       */
/*  nearby検索, 属性チェック, カウント, 範囲攻撃 (apply_area) を提供する。   */
/* ======================================================================== */

#include "libos32chem.h"
#include "libos32math.h"         /* fast_distance_sq */

/* chem_core.c の内部グローバルへのアクセサ */
extern ChemObject *chem__get_objects(void);

/* ====================================================================== */
/*  公開API: chem_has_element                                              */
/* ====================================================================== */

int chem_has_element(int obj_id, u32 elem)
{
    ChemObject *objects = chem__get_objects();

    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) return 0;
    if (objects[obj_id].id == 0) return 0;
    return (objects[obj_id].elements & elem) == elem;
}

/* ====================================================================== */
/*  公開API: chem_count_burning                                            */
/* ====================================================================== */

int chem_count_burning(void)
{
    ChemObject *objects = chem__get_objects();
    int i;
    int count = 0;

    for (i = 0; i < CHEM_MAX_OBJECTS; i++) {
        if (objects[i].id != 0 &&
            objects[i].state == CHEM_STATE_BURNING) {
            count++;
        }
    }
    return count;
}

/* ====================================================================== */
/*  公開API: chem_find_nearby                                              */
/* ====================================================================== */

int chem_find_nearby(i16 x, i16 y, int radius,
                     int *out_ids, int max_count)
{
    ChemObject *objects = chem__get_objects();
    u32 radius_sq;
    int i;
    int found = 0;

    if (!out_ids || max_count <= 0) return 0;

    radius_sq = (u32)((i32)radius * (i32)radius);

    for (i = 0; i < CHEM_MAX_OBJECTS && found < max_count; i++) {
        u32 dist_sq;
        if (objects[i].id == 0) continue;

        dist_sq = fast_distance_sq(
            (int)(objects[i].x - x),
            (int)(objects[i].y - y));

        if (dist_sq <= radius_sq) {
            out_ids[found++] = i;
        }
    }

    return found;
}

/* ====================================================================== */
/*  公開API: chem_apply_area                                               */
/* ====================================================================== */

int chem_apply_area(i16 x, i16 y, int radius,
                    u32 elements, i16 temp_delta)
{
    ChemObject *objects = chem__get_objects();
    u32 radius_sq;
    int i;
    int affected = 0;
    i32 t;

    radius_sq = (u32)((i32)radius * (i32)radius);

    for (i = 0; i < CHEM_MAX_OBJECTS; i++) {
        u32 dist_sq;
        ChemObject *obj;

        if (objects[i].id == 0) continue;

        obj = &objects[i];
        dist_sq = fast_distance_sq(
            (int)(obj->x - x),
            (int)(obj->y - y));

        if (dist_sq > radius_sq) continue;

        /* 属性を付与 */
        if (elements != ELEM_NONE) {
            obj->elements |= elements;
        }

        /* 温度変化 */
        if (temp_delta != 0) {
            t = (i32)obj->temperature + (i32)temp_delta;
            if (t > 32767) t = 32767;
            if (t < -32768) t = -32768;
            obj->temperature = (i16)t;
        }

        /* 火属性を付与された場合は着火 */
        if (elements & ELEM_FIRE) {
            obj->state = CHEM_STATE_BURNING;
        }

        /* 氷属性を付与された場合は凍結 */
        if (elements & ELEM_ICE) {
            obj->state = CHEM_STATE_FROZEN;
        }

        /* 電気属性を付与された場合は帯電 */
        if (elements & ELEM_ELECTRIC) {
            obj->state = CHEM_STATE_CHARGED;
        }

        affected++;
    }

    return affected;
}
