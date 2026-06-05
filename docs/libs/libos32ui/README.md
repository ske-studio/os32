# libos32ui — microUI (OS32移植版)

## 1. 概要

`libos32ui` は、rxi 氏によって開発された極めて軽量な即時モード（Immediate Mode）GUIライブラリ **`microUI`** を OS32 環境へ移植したものです。

ウィンドウ、ボタン、テキストラベル、スライダー、レイアウト管理などのGUIコントロールを提供し、ゲームのデバッグツール、設定画面、ツールアプリケーション、CUIから移行する簡易GUIデスクトップ環境などで容易にリッチなUIを構築できます。

---

## 2. アーキテクチャ

`microUI` 自体はプラットフォーム非依存の純粋なロジックライブラリであり、UI要素のレイアウトや状態遷移（フォーカス、クリック検出）を計算して抽象的な描画コマンドキューを生成します。

`libos32ui` は、このコマンドキューを受け取り、グラフィックスライブラリ `libos32gfx` および KCGキャッシュフォントを使用して、PC-98の画面（VRAM）へ実際に描画する「バックエンドブリッジ」の役割を果たします。

```
  [アプリケーション] (毎フレーム UI定義を記述)
         ↓ (microUI 即時モードAPI)
     [microUI] ──(レイアウト/入力判定)──→ [描画コマンドキュー]
                                                ↓
                                          [libos32ui]
                                                ↓ (GFXプリミティブ呼出)
                                          [libos32gfx] ──→ 画面
```

---

## 3. 主要API

### 3.1 システム管理
*   `void ui_init(KernelAPI *api)`
    *   UIシステムを初期化します。内部で `mu_Context` をセットアップします。
*   `void ui_shutdown()`
    *   UIシステムをクローズします。
*   `void ui_begin(void)`
    *   1フレームのUI処理を開始し、入力状態（マウス/キーボード）を反映させます。
*   `void ui_end(void)`
    *   UI処理を完了させ、生成された描画コマンドを画面（バックバッファ）にレンダリングします。

### 3.2 ウィンドウ・パネル
*   `int mu_begin_window(mu_Context *ctx, const char *title, mu_Rect rect)`
    *   ウィンドウを配置します。ドラッグによる移動や折りたたみに対応しています。
*   `void mu_end_window(mu_Context *ctx)`
    *   ウィンドウ定義を終了します。

### 3.3 ウィジェット（コントロール）
*   `void mu_label(mu_Context *ctx, const char *text)`: テキストを表示します。
*   `int mu_button(mu_Context *ctx, const char *label)`: ボタンを配置し、クリックされたら非0を返します。
*   `int mu_checkbox(mu_Context *ctx, const char *label, int *state)`: チェックボックスを配置します。
*   `int mu_textbox(mu_Context *ctx, char *buf, int buf_size)`: テキスト入力ボックスを配置します（キーボードフォーカス時に入力可能）。
*   `int mu_slider(mu_Context *ctx, mu_Real *val, mu_Real low, mu_Real high)`: スライダーを配置します。

---

## 4. 使用例

即時モードによるシンプルなウィンドウ定義の例：

```c
#include "libos32ui.h"
#include "libos32gfx.h"

void draw_my_gui(mu_Context *ctx) {
    /* ウィンドウを (40, 40) の位置にサイズ 200x150 で配置 */
    if (mu_begin_window(ctx, "Demo Window", mu_rect(40, 40, 200, 150))) {
        
        mu_layout_row(ctx, 1, (int[]) { -1 }, 0); // 1列レイアウト
        
        mu_label(ctx, "Hello, OS32 GUI!");
        
        if (mu_button(ctx, "Click Me")) {
            // ボタンがクリックされた時の処理
        }
        
        static int check_state = 0;
        mu_checkbox(ctx, "Enable Option", &check_state);
        
        mu_end_window(ctx);
    }
}

void main_loop(void) {
    mu_Context *ctx = mu_get_context(); // コンテキスト取得

    while (1) {
        gfx_clear(0);
        
        ui_begin();
        draw_my_gui(ctx);
        ui_end();
        
        gfx_present();
    }
}
```
