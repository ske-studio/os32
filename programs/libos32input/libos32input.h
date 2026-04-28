/* ======================================================================== */
/*  LIBOS32INPUT.H — OS32 入力抽象化ライブラリ 公開ヘッダ                    */
/*                                                                          */
/*  物理デバイス (キーボード・マウス・将来ゲームパッド) の入力を               */
/*  論理アクションに変換する抽象化ライブラリ。                                */
/*                                                                          */
/*  依存: libos32math (fix16_t), KernelAPI (kbd/mouse)                      */
/*  描画 (gfx/pyxel) には依存しない。                                        */
/* ======================================================================== */

#ifndef LIBOS32INPUT_H
#define LIBOS32INPUT_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32, KernelAPI */
#include "libos32math.h"         /* fix16_t, FIX16_ONE, fix16_mul, fix16_clamp */

/* ====================================================================== */
/*  1. デバイス種別                                                        */
/* ====================================================================== */

#define INP_DEV_KEYBOARD  0
#define INP_DEV_MOUSE     1
#define INP_DEV_GAMEPAD   2   /* 将来用: シリアルブリッジ経由 */

/* ====================================================================== */
/*  2. 配列上限                                                            */
/* ====================================================================== */

#define INPUT_MAX_ACTIONS   32   /* 同時管理アクション上限 */
#define INPUT_MAX_BINDINGS  64   /* 同時バインディング上限 */

/* ====================================================================== */
/*  3. アクション状態 (InputActionState)                                   */
/* ====================================================================== */

typedef struct {
    fix16_t value;              /* 現在の入力値 (0=未入力, FIX16_ONE=全押し) */
    fix16_t prev_value;         /* 前フレームの値 (トリガー/リリース判定用) */
    u16     hold_frames;        /* 押し続けフレーム数 (リピート判定用) */
    u16     _pad;
} InputActionState;

/* ====================================================================== */
/*  4. バインディング (InputBinding)                                       */
/* ====================================================================== */

typedef struct {
    u8      action_id;          /* 紐付け先アクションID (0-31) */
    u8      device;             /* INP_DEV_* */
    u16     code;               /* キーボード: スキャンコード (0x00-0x7F)
                                   マウス: MOUSE_BTN_LEFT 等 */
    fix16_t scale;              /* 入力値のスケール
                                   デジタル: FIX16_ONE or -FIX16_ONE
                                   アナログ: 任意の倍率 */
} InputBinding;                 /* 合計 8バイト */

/* ====================================================================== */
/*  5. マウスキャッシュ                                                    */
/* ====================================================================== */

typedef struct {
    i16 x, y;                   /* 現在座標 */
    i16 dx, dy;                 /* 前フレームからの差分 */
    u8  buttons;                /* ボタンビットマスク */
    u8  polled;                 /* このフレームでpoll済みか */
} InputMouseCache;

/* ====================================================================== */
/*  6. API — システム管理 (input_core.c)                                   */
/* ====================================================================== */

/* 初期化: KAPIポインタを保持、内部配列ゼロクリア */
int  input_init(KernelAPI *api);

/* 終了: 内部状態リセット */
void input_shutdown(void);

/* 毎フレーム1回呼ぶ (ゲームループの先頭)
 *
 * 処理内容:
 *   1. prev_value <- value をシフト
 *   2. 全アクションの value をリセット
 *   3. バインディング配列を走査し、KAPI経由で物理入力を取得
 *   4. scale を適用して value に合算
 *   5. value を -FIX16_ONE ~ FIX16_ONE にクランプ
 *   6. hold_frames を更新
 */
void input_update(void);

/* ====================================================================== */
/*  7. API — バインディング管理 (input_bind.c)                             */
/* ====================================================================== */

/* 物理入力→アクションの紐付けを追加
 * 戻り値: 0=成功, -1=バインディング上限超過
 *
 * 使用例:
 *   input_bind(ACT_JUMP, INP_DEV_KEYBOARD, 0x34, FIX16_ONE);   Space→Jump
 *   input_bind(ACT_JUMP, INP_DEV_MOUSE, MOUSE_BTN_LEFT, FIX16_ONE);
 *   input_bind(ACT_MOVE_X, INP_DEV_KEYBOARD, 0x3C, FIX16_ONE);  右→+1.0
 *   input_bind(ACT_MOVE_X, INP_DEV_KEYBOARD, 0x3B, -FIX16_ONE); 左→-1.0
 */
int  input_bind(int action_id, int device, int code, fix16_t scale);

/* 指定アクションの全バインディングを解除 */
void input_unbind(int action_id);

/* 全バインディングを解除 */
void input_unbind_all(void);

/* ====================================================================== */
/*  8. API — アクション状態取得 (input_query.c)                            */
/* ====================================================================== */

/* 現在押下中か (value != 0 なら非0を返す) */
int input_pressed(int action_id);

/* 押した瞬間か (前フレーム未入力→今フレーム入力) */
int input_triggered(int action_id);

/* 離した瞬間か (前フレーム入力→今フレーム未入力) */
int input_released(int action_id);

/* アナログ値取得 (-FIX16_ONE ~ FIX16_ONE)
 * デジタル入力の場合: 0 or FIX16_ONE (or -FIX16_ONE)
 * 左右同時押しの場合: 0 (合算でキャンセル)
 */
fix16_t input_value(int action_id);

/* リピート判定 (pyxel_btnp 互換)
 * hold: 最初のリピートまでのフレーム数 (0=リピートなし)
 * repeat: 以降のリピート間隔フレーム数
 */
int input_held(int action_id, int hold, int repeat);

/* ====================================================================== */
/*  9. API — ユーティリティ                                                */
/* ====================================================================== */

/* 修飾キー状態取得 (kbd_get_modifiers ラッパー)
 * 戻り値: SHIFT_SHIFT | SHIFT_CTRL | SHIFT_CAPS 等のビットマスク
 */
u32 input_modifiers(void);

/* マウス座標取得 (mouse_poll キャッシュから) */
void input_get_mouse(i16 *x, i16 *y);

/* マウス差分取得 */
void input_get_mouse_delta(i16 *dx, i16 *dy);

/* マウスボタン押下判定 (MOUSE_BTN_LEFT 等) */
int input_mouse_btn(int btn);

#endif /* LIBOS32INPUT_H */
