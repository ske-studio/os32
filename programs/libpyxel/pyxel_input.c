/* ======================================================================== */
/*  PYXEL_INPUT.C — libpyxel 入力管理                                       */
/*                                                                          */
/*  オンデマンド方式: pyxel_btn/btnp/btnr が呼ばれた時点で                   */
/*  kbd_is_pressed() を直接呼び出す。                                        */
/*  前フレームとの差分はフレーム開始時に一括更新する。                        */
/*                                                                          */
/*  128キー全ポーリング方式は KAPI呼出128回/フレームのオーバーヘッドが       */
/*  大きすぎるため (16MHz環境で顕著)、廃止した。                             */
/* ======================================================================== */

#include <string.h>
#include "pyxel_internal.h"

/* ======================================================================== */
/*  _pyxel_update_input — フレーム開始時の状態更新                           */
/*                                                                          */
/*  prev_keys ← cur_keys をシフトし、cur_keys をクリアする。                */
/*  実際のキー状態取得は pyxel_btn() 呼び出し時にオンデマンドで行う。        */
/* ======================================================================== */

void _pyxel_update_input(void)
{
    if (!_pyxel.kapi) return;

    /* prev_keys ← cur_keys */
    memcpy(_pyxel.prev_keys, _pyxel.cur_keys, PYXEL_KEY_BYTES);

    /* cur_keys をクリア (このフレームで pyxel_btn() が呼ばれた分だけ再設定される) */
    memset(_pyxel.cur_keys, 0, PYXEL_KEY_BYTES);

    /* ホールドカウンタも prev_keys ベースで更新 */
    {
        int i;
        for (i = 0; i < PYXEL_MAX_KEYS; i++) {
            if (PYXEL_KEY_TST(_pyxel.prev_keys, i)) {
                if (_pyxel.key_hold[i] < 0xFFFF) {
                    _pyxel.key_hold[i]++;
                }
            } else {
                _pyxel.key_hold[i] = 0;
            }
        }
    }
}

/* ======================================================================== */
/*  内部: 指定キーの現在状態を取得し cur_keys にキャッシュ                   */
/* ======================================================================== */

static int _pyxel_poll_key(int key)
{
    int pressed;

    if (!_pyxel.kapi) return 0;

    pressed = _pyxel.kapi->kbd_is_pressed(key);
    if (pressed) {
        PYXEL_KEY_SET(_pyxel.cur_keys, key);
    }
    return pressed;
}

/* ======================================================================== */
/*  pyxel_btn — キー押下状態                                                 */
/* ======================================================================== */

int pyxel_btn(int key)
{
    if (key < 0 || key >= PYXEL_MAX_KEYS) return 0;

    /* cur_keys にキャッシュ済みかチェック */
    if (PYXEL_KEY_TST(_pyxel.cur_keys, key)) return 1;

    /* まだ未取得 → KAPI経由で取得 */
    return _pyxel_poll_key(key);
}

/* ======================================================================== */
/*  pyxel_btnp — トリガー判定 + リピート                                     */
/* ======================================================================== */

int pyxel_btnp(int key, int hold, int repeat)
{
    int cur, prev;
    u16 h;

    if (key < 0 || key >= PYXEL_MAX_KEYS) return 0;

    /* 現在の状態を取得 */
    cur  = pyxel_btn(key);
    prev = PYXEL_KEY_TST(_pyxel.prev_keys, key) ? 1 : 0;

    /* 押された瞬間 (立ち上がりエッジ) */
    if (cur && !prev) return 1;

    /* リピート処理 */
    if (hold > 0 && repeat > 0 && cur) {
        h = _pyxel.key_hold[key];
        if (h > (u16)hold) {
            if (((h - hold) % repeat) == 0) return 1;
        }
    }

    return 0;
}

/* ======================================================================== */
/*  pyxel_btnr — リリース判定                                                */
/* ======================================================================== */

int pyxel_btnr(int key)
{
    int cur, prev;

    if (key < 0 || key >= PYXEL_MAX_KEYS) return 0;

    cur  = pyxel_btn(key);
    prev = PYXEL_KEY_TST(_pyxel.prev_keys, key) ? 1 : 0;

    return (!cur && prev) ? 1 : 0;
}
