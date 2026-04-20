# 06: libpyxel APIリファレンスとモジュール構成

本ドキュメントでは、OS32環境でPyxel互換のC APIを提供する「libpyxel」について、必要な関数プロトタイプ、定数群、およびソースファイルのディレクトリ構造を取りまとめる。

## 1. ディレクトリ構造とソースファイル一覧

libpyxel本体、変換ツール、および関連アプリのディレクトリ構成は以下の通りとする。

```text
src/os32/
├── tools/
│   └── pyxres2os32.py         ... (Phase 1) .pyxres から .os32res へのリソース変換ツール
│
└── programs/
    ├── libpyxel/              ... libpyxel 本体
    │   ├── Makefile           ... libpyxel のビルドスクリプト
    │   ├── pyxel.h            ... 公開APIヘッダ (全関数、定数、マクロを定義)
    │   ├── pyxel_internal.h   ... 内部状態管理・リソース用ヘッダ
    │   ├── pyxel_core.c       ... (Phase 2) init, run, quit, 画面更新
    │   ├── pyxel_gfx.c        ... (Phase 2,3) cls, pset, line, rect, circ, tri, fill, blt 等
    │   ├── pyxel_input.c      ... (Phase 4) btn, btnp 等の入力管理機能
    │   ├── pyxel_snd.c        ... (Phase 4) play, playm 等のオーディオ機能
    │   └── pyxel_res.c        ... (Phase 3) load (.os32res パーサ)
    │
    ├── tests/
    │   └── bench_scale2x/     ... (Phase 0) スケーリング転送性能計測用ベンチマーク
    │       ├── main.c
    │       └── Makefile
    │
    └── apps/
        └── pyxeldemo/         ... (Phase 5) libpyxel 動作検証用のデモアプリ
            ├── main.c         ... Pyxelゲームの移植デモコード
            ├── Makefile
            └── assets/        ... リソース (.os32res)
```

## 2. 定数・データ型定義 (`pyxel.h`)

Pyxel準拠の定数をC89で表現する。

### 2-1. システム定数

```c
/* 基本画面サイズ */
#define PYXEL_WIDTH  256
#define PYXEL_HEIGHT 192

/* 内部カラーパレット数 */
#define PYXEL_COLORS 16

/* 表示倍率 */
#define PYXEL_SCALE  2

/* 表示解像度 */
#define PYXEL_DISP_WIDTH   (PYXEL_WIDTH * PYXEL_SCALE)    /* 512 */
#define PYXEL_DISP_HEIGHT  (PYXEL_HEIGHT * PYXEL_SCALE)   /* 384 */
```

### 2-2. キーボード定数

Pyxel互換のキーボードスキャンコード定数（OS32のSys_KbdScanCodeベース）。

```c
#define PYXEL_KEY_UP     0x3A
#define PYXEL_KEY_DOWN   0x3D
#define PYXEL_KEY_LEFT   0x3B
#define PYXEL_KEY_RIGHT  0x3C
#define PYXEL_KEY_SPACE  0x34
#define PYXEL_KEY_RETURN 0x1C
#define PYXEL_KEY_ESCAPE 0x00
#define PYXEL_KEY_Z      0x2A
#define PYXEL_KEY_X      0x2B
/* 実装時に他キーも適宜追加 */
```

### 2-3. サウンド定数

```c
/* 波形種別 (tones) */
#define PYXEL_TONE_TRIANGLE  0
#define PYXEL_TONE_SQUARE    1
#define PYXEL_TONE_PULSE     2
#define PYXEL_TONE_NOISE     3

/* エフェクト種別 (effects) */
#define PYXEL_EFFECT_NONE          0
#define PYXEL_EFFECT_SLIDE         1
#define PYXEL_EFFECT_VIBRATO       2
#define PYXEL_EFFECT_FADEOUT       3
#define PYXEL_EFFECT_HALF_FADEOUT  4
#define PYXEL_EFFECT_QUARTER_FADEOUT 5

/* サウンドタイミング */
#define PYXEL_TICK_HZ  120   /* 1 tick = 1/120 秒 */
```

### 2-4. グローバル変数

Pyxel互換の公開グローバル変数（C APIとしてextern宣言）。

```c
/* 起動してからの総フレーム数 (pyxel_run内でインクリメントされる) */
extern unsigned int pyxel_frame_count;
```

---

## 3. 関数プロトタイプ (`pyxel.h`)

C89/OS32環境向けに設計した libpyxel の公開関数宣言。

### 3-1. システム管理 (pyxel_core.c)

```c
/* 初期化 (GFX初期化やPyxel公式パレット設定) */
void pyxel_init(int width, int height);

/* メインループ (コールバック関数を登録) */
void pyxel_run(void (*update)(void), void (*draw)(void));

/* 終了処理 */
void pyxel_quit(void);
```

### 3-2. リソース管理 (pyxel_res.c)

```c
/* 変換済みの .os32res ファイルをロード・メモリ展開 */
void pyxel_load(const char *filename);
```

### 3-3. グラフィック描画 (pyxel_gfx.c)

```c
/* 画面クリア */
void pyxel_cls(int col);

/* ピクセル描画/取得 */
void pyxel_pset(int x, int y, int col);
int pyxel_pget(int x, int y);

/* プリミティブ描画 — 矩形 */
void pyxel_rect(int x, int y, int w, int h, int col);   /* 塗りつぶし */
void pyxel_rectb(int x, int y, int w, int h, int col);  /* 枠のみ */

/* プリミティブ描画 — 線 */
void pyxel_line(int x1, int y1, int x2, int y2, int col);

/* プリミティブ描画 — 円 */
void pyxel_circ(int x, int y, int r, int col);   /* 塗りつぶし */
void pyxel_circb(int x, int y, int r, int col);  /* 枠のみ */

/* プリミティブ描画 — 三角形 */
void pyxel_tri(int x1, int y1, int x2, int y2, int x3, int y3, int col);   /* 塗りつぶし */
void pyxel_trib(int x1, int y1, int x2, int y2, int x3, int y3, int col);  /* 枠のみ */

/* フラッドフィル (塗りつぶし) */
void pyxel_fill(int x, int y, int col);

/* 画像・タイル転送機能 */
/* img: バンク(0-2), u/v: 元画像座標, w/h: サイズ(負で反転), colkey: 透過色 */
void pyxel_blt(int x, int y, int img, int u, int v, int w, int h, int colkey);

/* tm: タイルマップ(0-7) */
void pyxel_bltm(int x, int y, int tm, int u, int v, int w, int h, int colkey);

/* 組み込みテキスト描画 (OS32組み込みANKフォント) */
void pyxel_text(int x, int y, const char *s, int col);

/* パレットスワップ設定 */
void pyxel_pal(int col1, int col2);

/* パレットをデフォルトにリセット */
void pyxel_pal_reset(void);

/* カメラ (描画オフセット) 設定 */
void pyxel_camera(int x, int y);

/* クリッピング領域設定 (引数なしでリセット) */
void pyxel_clip(int x, int y, int w, int h);

/* クリッピングをリセット (全画面) */
void pyxel_clip_reset(void);
```

### 3-4. 入力管理 (pyxel_input.c)

```c
/* キー押下状態 (押下中なら非0) */
int pyxel_btn(int key);

/* 押された瞬間のトリガー判定 (hold/repeatフレームでのリピート対応) */
int pyxel_btnp(int key, int hold, int repeat);

/* 離された瞬間のリリース判定 */
int pyxel_btnr(int key);
```

### 3-5. オーディオ管理 (pyxel_snd.c)

```c
/* 指定チャンネルでサウンド再生 (ch:0-2はFM, ch:3はSSG) */
void pyxel_play(int ch, int snd);

/* ミュージック(BGM)全体を再生 */
void pyxel_playm(int msc);

/* 指定チャンネル(または全チャンネル)の再生停止 */
void pyxel_stop(int ch);
```
