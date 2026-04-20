# 04: Pyxel API → OS32 C API マッピング仕様

## 1. Pyxel API 一覧と OS32 対応方針

### 1-1. システム

| Pyxel (Python) | libpyxel (C89) | OS32 バックエンド | 備考 |
|----------------|-----------------|-------------------|------|
| `pyxel.init(w, h, title)` | `pyxel_init(w, h)` | `gfx_init()` + パレット設定 | titleはOS32上では不使用 |
| `pyxel.run(update, draw)` | `pyxel_run(update, draw)` | メインループ + VSYNC待ち | 関数ポインタ |
| `pyxel.quit()` | `pyxel_quit()` | `gfx_shutdown()` | - |
| `pyxel.width` / `pyxel.height` | `PYXEL_WIDTH` / `PYXEL_HEIGHT` | 定数 (256, 192) | - |
| `pyxel.frame_count` | `pyxel_frame_count` | `tick_count` から算出 | グローバル変数 |

### 1-2. リソース

| Pyxel (Python) | libpyxel (C89) | OS32 バックエンド | 備考 |
|----------------|-----------------|-------------------|------|
| `pyxel.load(filename)` | `pyxel_load(path)` | `sys_open` + `sys_read` | `.os32res` をロード |
| `pyxel.image(n)` | 内部配列アクセス | プレーナーデータポインタ | バンク 0-2 |
| `pyxel.tilemap(n)` | 内部配列アクセス | タイル参照テーブル | マップ 0-7 |
| `pyxel.sound(n)` | 内部配列アクセス | サウンドデータ構造体 | 0-63 |
| `pyxel.music(n)` | 内部配列アクセス | ミュージックデータ構造体 | 0-7 |

### 1-3. グラフィック描画

| Pyxel (Python) | libpyxel (C89) | OS32 バックエンド | 備考 |
|----------------|-----------------|-------------------|------|
| `pyxel.cls(col)` | `pyxel_cls(col)` | `gfx_clear_rect()` | ゲーム領域のみクリア |
| `pyxel.pset(x,y,col)` | `pyxel_pset(x,y,col)` | `gfx_fill_rect(x*2,y*2,2,2,col)` | 2倍座標変換 |
| `pyxel.pget(x,y)` | `pyxel_pget(x,y)` | `gfx_get_pixel(x*2,y*2)` | BB読み出し |
| `pyxel.line(x1,y1,x2,y2,col)` | `pyxel_line(...)` | ブレゼンハム + pset | 2倍座標 |
| `pyxel.rect(x,y,w,h,col)` | `pyxel_rect(...)` | `gfx_fill_rect()` 2倍座標 | **塗りつぶし矩形** |
| `pyxel.rectb(x,y,w,h,col)` | `pyxel_rectb(...)` | `gfx_rect()` 2倍座標 | **枠のみ矩形** |
| `pyxel.circ(x,y,r,col)` | `pyxel_circ(...)` | `gfx_fill_circle()` 2倍 | 塗り円 |
| `pyxel.circb(x,y,r,col)` | `pyxel_circb(...)` | `gfx_circle()` 2倍 | 枠円 |
| `pyxel.tri(x1,y1,x2,y2,x3,y3,col)` | `pyxel_tri(...)` | 塗り三角形 (要実装) | 塗りつぶし三角形 |
| `pyxel.trib(x1,y1,x2,y2,x3,y3,col)` | `pyxel_trib(...)` | 枠三角形 (3本line) | 枠三角形 |
| `pyxel.fill(x,y,col)` | `pyxel_fill(x,y,col)` | フラッドフィル (要実装) | 塗りつぶし |
| `pyxel.blt(x,y,img,u,v,w,h,colkey)` | `pyxel_blt(...)` | プレーナーマスクコピー | スプライト描画 |
| `pyxel.bltm(x,y,tm,u,v,w,h,colkey)` | `pyxel_bltm(...)` | タイルマップ描画 | タイル参照+blt |
| `pyxel.text(x,y,s,col)` | `pyxel_text(...)` | `kcg_draw_ank()` | Pyxelフォントは4×6 |
| `pyxel.pal(c1,c2)` | `pyxel_pal(c1,c2)` | パレットスワップテーブル | ソフトウェア実装 |
| `pyxel.pal()` (引数なし) | `pyxel_pal_reset()` | パレットリセット | デフォルトに戻す |
| `pyxel.camera(x,y)` | `pyxel_camera(x,y)` | 描画オフセット設定 | スクロールに使用 |
| `pyxel.clip(x,y,w,h)` | `pyxel_clip(x,y,w,h)` | クリッピング領域設定 | 描画範囲制限 |

> **注意**: `blt`/`bltm` には Pyxel v2.2.0 で `rotate`, `scale` オプションが追加
> されているが、OS32では計算コストが大きいため Phase 1 では省略する。

### 1-4. 入力

| Pyxel (Python) | libpyxel (C89) | OS32 バックエンド | 備考 |
|----------------|-----------------|-------------------|------|
| `pyxel.btn(key)` | `pyxel_btn(key)` | `sys_kbd_get_state()` | 押下状態 |
| `pyxel.btnp(key,hold,rep)` | `pyxel_btnp(key,hold,rep)` | フレーム差分検出 | トリガー+リピート |
| `pyxel.btnr(key)` | `pyxel_btnr(key)` | フレーム差分検出 | リリース検出 |

### 1-5. オーディオ

| Pyxel (Python) | libpyxel (C89) | OS32 バックエンド | 備考 |
|----------------|-----------------|-------------------|------|
| `pyxel.play(ch,snd)` | `pyxel_play(ch,snd)` | FM/SSGチャンネル振り分け | ch0-3 |
| `pyxel.playm(msc)` | `pyxel_playm(msc)` | BGMシーケンサ起動 | `snd_bgm_play` 相当 |
| `pyxel.stop(ch)` | `pyxel_stop(ch)` | `fm_note_off` / `ssg_all_off` | - |

## 2. キーマッピング

Pyxelのキー定数をOS32のスキャンコードにマッピングする。

| Pyxel Key | 値 | PC-98 スキャンコード | 備考 |
|-----------|----|---------------------|------|
| KEY_UP | 定義要 | 0x3A | テンキーの↑も可 |
| KEY_DOWN | 定義要 | 0x3D | |
| KEY_LEFT | 定義要 | 0x3B | |
| KEY_RIGHT | 定義要 | 0x3C | |
| KEY_SPACE | 定義要 | 0x34 | |
| KEY_RETURN | 定義要 | 0x1C | |
| KEY_ESCAPE | 定義要 | 0x00 | |
| KEY_Z | 定義要 | 0x2A | ゲームボタンA |
| KEY_X | 定義要 | 0x2B | ゲームボタンB |
| KEY_A〜KEY_Z | 定義要 | 各キー | 汎用 |

## 3. パレットマッピング

Pyxelの16色パレット (8bit RGB) を PC-98の4bitアナログパレットに変換する。

### 3-1. 減色方式

PC-98 アナログパレット: 各色 R/G/B 4bit (0-15)、計 4096色から16色選択。
Pyxelパレット: 各色 R/G/B 8bit。

変換式: `pc98_val = (pyxel_val * 15 + 127) / 255` (四捨五入)

| Pyxel Index | Pyxel RGB (8bit) | PC-98 RGB (4bit) | 色名 |
|-------------|-----------------|-------------------|------|
| 0 | (0, 0, 0) | (0, 0, 0) | 黒 |
| 1 | (43, 51, 95) | (3, 3, 6) | 紺 |
| 2 | (126, 32, 114) | (7, 2, 7) | 紫 |
| 3 | (25, 149, 156) | (1, 9, 9) | 青緑 |
| 4 | (139, 72, 82) | (8, 4, 5) | 茶 |
| 5 | (57, 92, 152) | (3, 5, 9) | 暗青 |
| 6 | (169, 193, 255) | (10, 11, 15) | 薄青 |
| 7 | (238, 238, 238) | (14, 14, 14) | 白 |
| 8 | (212, 24, 108) | (12, 1, 6) | 赤 |
| 9 | (211, 132, 65) | (12, 8, 4) | 橙 |
| 10 | (233, 195, 91) | (14, 11, 5) | 黄 |
| 11 | (112, 198, 169) | (7, 12, 10) | 薄緑 |
| 12 | (118, 150, 222) | (7, 9, 13) | シアン |
| 13 | (163, 163, 163) | (10, 10, 10) | 灰 |
| 14 | (255, 151, 152) | (15, 9, 9) | ピンク |
| 15 | (237, 199, 176) | (14, 12, 10) | 肌色 |

`pyxel_init()` 時に `palette_set()` でこの16色を設定する。

## 4. サウンドマッピング

### 4-1. Pyxel サウンドチャンネル → PC-98 音源チャンネル

```text
Pyxel Ch0 → FM Ch0 (YM2203)
Pyxel Ch1 → FM Ch1
Pyxel Ch2 → FM Ch2
Pyxel Ch3 → SSG Ch A (YM2203 PSG部)
```

### 4-2. Pyxel 波形 → PC-98 音色マッピング

| Pyxel Tone | 波形 | PC-98 実装 |
|-----------|------|-----------| 
| 0 (Triangle) | 三角波 | FM: オルガン音色 (`tone_organ`) |
| 1 (Square) | 矩形波 | SSG: 矩形波 (ネイティブ) |
| 2 (Pulse) | パルス波 | FM: ピアノ音色 (`tone_piano`) |
| 3 (Noise) | ノイズ | SSG: ノイズモード |

### 4-3. Pyxel ノート → PC-98 ノート変換

Pyxelのノート番号 (0-59) は `base + octave × 12` 形式。
FM音源の `fm_note_on()` も同じ形式 (`NOTE(oct, key)`) を使用しているため、
直接マッピング可能。

```c
void pyxel_play_note(int ch, int note, int tone)
{
    if (ch < 3) {
        /* FM チャンネル */
        fm_set_tone_num(ch, tone_map[tone]);
        fm_note_on(ch, note);
    } else {
        /* SSG チャンネル */
        /* ノート番号からSSG周期に変換 */
        u16 period = ssg_note_to_period(note);
        ssg_tone(ch - 3, period);
        ssg_volume(ch - 3, 15);
    }
}
```

### 4-4. エフェクト対応

| Pyxel Effect | 動作 | Phase 1 対応 |
|-------------|------|-------------|
| 0 (None) | なし | ○ |
| 1 (Slide) | 音程スライド | × (将来対応) |
| 2 (Vibrato) | ビブラート | × (将来対応) |
| 3 (FadeOut) | フェードアウト | △ (音量漸減で近似) |
| 4 (Half-FadeOut) | 半フェードアウト | × (将来対応) |
| 5 (Quarter-FadeOut) | 1/4フェードアウト | × (将来対応) |

### 4-5. 再生速度

Pyxelのサウンド `speed` は **tick/note** 単位で、**1 tick = 1/120 秒**。
OS32のタイマー割り込みは `tick_count` (100Hz) であるため、近似的に:
- Pyxel speed 30 → 1ノート = 30/120 = 0.25秒 → OS32 tick 25回
- 変換式: `os32_ticks = pyxel_speed * 100 / 120`

## 5. ゲームループ構造

```c
/* libpyxel メインループの内部実装 */
void pyxel_run(void (*update)(void), void (*draw)(void))
{
    u32 last_tick = tick_count;

    for (;;) {
        /* 入力状態の更新 */
        _pyxel_update_input();

        /* ESCキーで終了 */
        if (pyxel_btn(KEY_ESCAPE)) break;

        /* ユーザーのupdate関数呼び出し */
        update();

        /* ユーザーのdraw関数呼び出し */
        draw();

        /* フレームカウンタ更新 */
        pyxel_frame_count++;

        /* VRAM転送 (ゲーム領域の2倍スケーリング) */
        _pyxel_present();

        /* フレームレート制御 (VSYNC or tick待ち) */
        while (tick_count == last_tick) {
            __asm__ volatile("hlt");
        }
        last_tick = tick_count;
    }
}
```

## 6. ヘッダファイル構造

```text
programs/libpyxel/
  pyxel.h            - 公開API (全関数プロトタイプ、定数)
  pyxel_internal.h   - 内部構造体 (リソースデータ、状態管理)
  pyxel_core.c       - init/run/quit
  pyxel_gfx.c        - cls/pset/line/rect/circ/tri/fill/blt/bltm/text/pal/camera/clip
  pyxel_input.c      - btn/btnp/btnr (フレーム差分管理)
  pyxel_snd.c        - play/playm/stop (シーケンサ)
  pyxel_res.c        - load (.os32res パーサ)
```
