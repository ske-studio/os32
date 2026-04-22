/* ======================================================================== */
/*  MOUSE_SEAMLESS.C — NP21/Wシームレスマウスドライバ                       */
/*                                                                          */
/*  NP21/Wの np2sysp ポート (0x7EF/0x7ED) 経由で getmpos コマンドを         */
/*  発行し、ホストウィンドウ上の絶対座標を取得する。                        */
/*                                                                          */
/*  プロトコルは NP21/W NT カーネルドライバ (npmouse/w2k/npmouse.c) の       */
/*  SendNP2GetMousePos() と同等。mode=0 (文字列レスポンス) を使用。         */
/*                                                                          */
/*  出典: NP21/W io/np2sysp.c (np2sysp_getmpos),                           */
/*        NP21/W np2tool/npmouse/w2k/npmouse.c (SendNP2GetMousePos)         */
/* ======================================================================== */

#include "mouse.h"
#include "np2sysp.h"
#include "io.h"

/* ====================================================================== */
/*  seamless_mouse_init — シームレスモード初期化                            */
/*                                                                          */
/*  NP21/Wが getmpos コマンドをサポートしているか確認する。                 */
/*  戻り値: 1=成功, 0=非対応                                               */
/* ====================================================================== */
int seamless_mouse_init(void)
{
    char buf[32];
    int len;

    /* mode=0 パラメータ送信 (4バイト、全て0) */
    outp(NP2PORT_VAL, 0x00);
    outp(NP2PORT_VAL, 0x00);
    outp(NP2PORT_VAL, 0x00);
    outp(NP2PORT_VAL, 0x00);

    /* getmpos コマンド送信 */
    np2_send_cmd("getmpos");

    /* レスポンス受信 */
    len = np2_recv_str(buf, sizeof(buf));

    /* 空文字列 = エラー/非対応 */
    if (len == 0) return 0;

    /* "数字,数字" 形式なら成功 */
    return 1;
}

/* ====================================================================== */
/*  parse_decimal — 文字列から10進数を読取り                               */
/*                                                                          */
/*  戻り値: パースした数値、*pos は次の位置に更新                          */
/* ====================================================================== */
static int parse_decimal(const char *str, int *pos)
{
    int val = 0;
    while (str[*pos] >= '0' && str[*pos] <= '9') {
        val = val * 10 + (str[*pos] - '0');
        (*pos)++;
    }
    return val;
}

/* ====================================================================== */
/*  seamless_mouse_poll — 絶対座標取得                                     */
/*                                                                          */
/*  getmpos mode=0 を実行し、"X,Y" レスポンスをパースする。                */
/*  座標は 0-65535 の正規化値で返される。                                   */
/*                                                                          */
/*  戻り値: 1=成功, 0=失敗                                                 */
/* ====================================================================== */
int seamless_mouse_poll(i16 *abs_x, i16 *abs_y)
{
    char buf[32];
    int len;
    int pos;
    int x_raw, y_raw;

    /* 古いレスポンスがあれば掃く */
    {
        int i;
        for (i = 0; i < 64; i++) {
            u8 ch = (u8)inp(NP2PORT_STR);
            if (ch == 0) break;
        }
    }

    /* mode=0 パラメータ送信 */
    outp(NP2PORT_VAL, 0x00);
    outp(NP2PORT_VAL, 0x00);
    outp(NP2PORT_VAL, 0x00);
    outp(NP2PORT_VAL, 0x00);

    /* getmpos コマンド送信 */
    np2_send_cmd("getmpos");

    /* レスポンス受信 "X,Y" */
    len = np2_recv_str(buf, sizeof(buf));
    if (len == 0) return 0;

    /* パース: "数値,数値" */
    pos = 0;
    x_raw = parse_decimal(buf, &pos);
    if (buf[pos] != ',') return 0;
    pos++;  /* ',' をスキップ */
    y_raw = parse_decimal(buf, &pos);

    /* 0-65535 → 画面座標に変換 */
    /* 640x400モード: x = raw * 639 / 65535, y = raw * 399 / 65535 */
    *abs_x = (i16)((long)x_raw * 639L / 65535L);
    *abs_y = (i16)((long)y_raw * 399L / 65535L);

    return 1;
}
