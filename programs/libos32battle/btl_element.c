/* ======================================================================== */
/*  BTL_ELEMENT.C — libos32battle 属性相性計算                              */
/*                                                                          */
/*  RAMキャッシュされた属性相性テーブルから倍率を検索する。                  */
/*  攻撃属性と防御属性のビットフラグをマッチングし、最初に一致した           */
/*  ペアの倍率を返す。複数属性を持つ場合は最も有利な倍率を採用。           */
/* ======================================================================== */

#include "libos32battle.h"

/* ====================================================================== */
/*  外部参照 (btl_core.c の内部アクセサ)                                    */
/* ====================================================================== */

extern BtlElementEntry *btl_get_elements(void);
extern int              btl_get_element_count(void);

/* ====================================================================== */
/*  btl_element_multiplier — 属性相性倍率を取得                            */
/*                                                                          */
/*  攻撃属性 (atk_elem) と防御属性 (def_elem) のビットフラグを              */
/*  属性相性テーブルと照合し、該当する倍率を返す。                          */
/*                                                                          */
/*  複数の属性ペアがマッチする場合:                                          */
/*    - 最も高い倍率 (最も有利な相性) を採用                                */
/*                                                                          */
/*  どのペアにもマッチしない場合: BTL_ELEM_NEUTRAL (256 = 等倍) を返す。     */
/* ====================================================================== */

i16 btl_element_multiplier(u32 atk_elem, u32 def_elem)
{
    BtlElementEntry *elems;
    int count;
    int i;
    i16 best;
    int found;

    /* 属性なし → 等倍 */
    if (atk_elem == 0 || def_elem == 0) {
        return BTL_ELEM_NEUTRAL;
    }

    elems = btl_get_elements();
    count = btl_get_element_count();

    if (count == 0) {
        return BTL_ELEM_NEUTRAL;
    }

    best = BTL_ELEM_NEUTRAL;
    found = 0;

    for (i = 0; i < count; i++) {
        /* 攻撃属性ビットが一致し、かつ防御属性ビットも一致する場合 */
        if ((atk_elem & elems[i].elem_atk) && (def_elem & elems[i].elem_def)) {
            if (!found || elems[i].multiplier > best) {
                best = elems[i].multiplier;
            }
            found = 1;
        }
    }

    return best;
}
