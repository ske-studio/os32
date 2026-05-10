# KernelAPI v38 — 補足ノート

[← KAPI_SPEC.md に戻る](KAPI_SPEC.md) | [APIテーブル](KAPI_TABLE.md)

機能グループ別の詳細説明。

---

## §5-1 グラフィックスAPI に関する補足

v22以降、基本的な描画プリミティブ (`gfx_clear`, `gfx_pixel`, `gfx_hline`, `gfx_vline`, `gfx_line`, `gfx_rect`, `gfx_fill_rect`) は KernelAPI から**廃止**されました。

外部プログラムでグラフィックス描画を行う場合は、以下の２つの方式から選択します:

1. **libos32gfx ライブラリ** (推奨): `programs/libos32gfx/` で提供されるスタティックリンクライブラリ。サーフェス、スプライト、描画プリミティブ、ダーティ矩形管理、フォントレンダリングなど高レベルな描画機能を提供します。
2. **フレームバッファ直接操作**: `gfx_get_framebuffer()` で取得した `GFX_Framebuffer` 構造体を介して、4プレーンのバックバッファに直接書き込み、`gfx_add_dirty_rect()` + `gfx_present_dirty()` でVRAMに転送します。

**描画モード**:

| モード | 解像度 | 初期化 | ページフリップ |
|--------|--------|--------|---------------|
| 400ラインモード | 640×400 | `gfx_init()` | 自動有効 |
| 200ラインモード | 640×200 | `gfx_init_200()` | 自動有効 |

**ページフリッピング**:
`gfx_init()` / `gfx_init_200()` いずれでもページフリッピングが自動的に有効になります。
`gfx_present_dirty()` / `gfx_present_nosync()` は非表示ページにVRAM転送後、ポートA4H/A6Hでページを切り替えます。VSYNC待ちは不要となり、ティアリングが発生しません。外部プログラム側のコード変更は不要です。

---

## §5-2 ラスタパレット (gfx_present_raster)

v24で追加。VSYNC後のアクティブ表示期間中に、走査線ごとにパレットレジスタを書き換えることで、16色パレットの制約を超えた擬似多色表示を実現します。

- **引数**: `GFX_RasterPalTable *table` — ラスタパレットテーブルへのポインタ
- **構造体**: `GFX_RasterPalEntry` (line, pal_idx, r, g, b) × 最大200エントリ
- **動作**: dirty rectがあればVRAM転送も行い、なければパレット書き換えのみ
- **ページフリップとの併用**: フリップモードではVRAM転送をフリップ経由で行い、
  VSYNC同期のパレット書き換えのみ実行します。両モードで動作します。
- **libos32gfx ラッパー**: `gfx_raster_clear()`, `gfx_raster_add()`, `gfx_present_raster_only()`, `gfx_present_with_raster()`

---

## §5-3 FDリダイレクト・パイプAPI

v25で追加。外部プログラム（シェル）がFD単位の入出力リダイレクトとパイプラインを構築するためのAPI群。

**FDリダイレクト**:
- `sys_redirect_fd(fd, path, mode)` — 指定FDの出力先をファイルにリダイレクト
- `sys_reset_redirect(fd)` — リダイレクトを解除しコンソールに復帰
- `sys_is_redirected(fd)` — FDがリダイレクト中か判定
- `sys_redirect_fd_buf(fd, buf, size, len)` — FDの出力先をメモリバッファにリダイレクト
- `sys_redirect_get_buf_len(fd)` — バッファリダイレクト時の書き込み済みバイト数取得

**パイプバッファ**:
- `sys_pipe_alloc()` — パイプバッファを1個確保 (IDを返す)
- `sys_pipe_free(id)` — パイプバッファを解放
- `sys_pipe_get_buf(id)` — パイプバッファのデータポインタ取得
- `sys_pipe_get_len(id)` — パイプバッファの書き込み済みバイト数取得
- `sys_pipe_clear(id)` — パイプバッファをクリア

**典型的なパイプ実行フロー** (`cmd1 | cmd2`):
1. `sys_pipe_alloc()` でパイプ確保
2. `sys_redirect_fd_buf(1, pipe_buf, size, 0)` でcmd1のstdoutをパイプに接続
3. cmd1を実行
4. `sys_reset_redirect(1)` でstdout復帰
5. `sys_redirect_fd_buf(0, pipe_buf, len, len)` でcmd2のstdinをパイプに接続
6. cmd2を実行
7. `sys_reset_redirect(0)` → `sys_pipe_free(id)` でクリーンアップ

---

## §5-4 ページング問い合わせAPI

v26で追加。指定アドレスのページテーブルエントリが存在するか (Present ビット) を確認する。

- `paging_is_present(addr)` — 指定アドレスが有効にマッピングされているか判定 (1=有効, 0=Not-Present)
- **用途**: メモリダンプツール等がガードページや未マッピング領域への不正アクセスを事前に回避するために使用

---

## §5-5 キー押下状態ポーリングAPI

v27で追加。指定スキャンコードのキーが現在押下中かをリアルタイムに問い合わせる。
ゲームエンジン (libpyxel) のフレーム単位入力に使用。

- `kbd_is_pressed(scancode)` — 指定スキャンコードのキーが押されていれば1、離されていれば0
- IRQハンドラで128キー分のビットマップを常時更新しているため、イベントキューを消費しない

---

## §5-6 FM/SSG個別チャンネル制御API

v27で追加。FM音源(YM2203)の3チャンネルおよびSSG(PSG)の3チャンネルを個別に制御する低レベルAPI。
ゲームエンジンのサウンドシーケンサ実装に使用。

- `fm_note_on(ch, note)` — FMチャンネル(0-2)でノート発音
- `fm_note_off(ch)` — FMチャンネル消音
- `fm_set_tone_num(ch, tone_num)` — FMチャンネルのプリセット音色設定
- `ssg_tone(ch, period)` — SSGチャンネル(0-2)のトーン周期設定
- `ssg_volume(ch, vol)` — SSGチャンネルの音量設定(0-15)
- `ssg_all_off()` — SSG全チャンネル消音

---

## §5-7 マウスAPI

v28で追加。PC-98バスマウスおよびNP21/Wシームレスマウスに対応するポーリングベースのマウスAPI。

- `mouse_poll(info)` — `MouseInfo` 構造体に現在の座標・差分・ボタン状態を取得
- `mouse_available()` — マウスが使用可能か判定 (1=バスマウス, 2=シームレス, 0=なし)
- `mouse_set_bounds(x_min, y_min, x_max, y_max)` — マウス座標のクランプ範囲を設定

**MouseInfo 構造体**:
- `x`, `y` — 現在の画面座標
- `dx`, `dy` — 前回poll以降の差分
- `buttons` — ボタンビットマスク (`MOUSE_BTN_LEFT`=0x01, `MOUSE_BTN_RIGHT`=0x02, `MOUSE_BTN_MIDDLE`=0x04)
- `mode` — 動作モード (0=なし, 1=バス, 2=シームレス)

---

## §5-8 TVRAM読取・反転API

v28で追加。テキストVRAMの読み取りと属性操作を行う。マウスカーソル (テキストモード) の実装に使用。

- `tvram_readchar_at(x, y, *code, *attr)` — TVRAM 1セルの文字コード＋属性を読み取る
- `tvram_reverse_cell(x, y)` — 属性反転トグル (PC-98属性ビット2 (0x04) のXOR)。漢字2セル自動対応。戻り値=セル幅 (ANK=1, 漢字=2)

---

## §5-9 マウスカーソル制御API

v28で追加。カーネル管理のマウスカーソル表示を制御する。アプリケーションはカーソル描画を自前で行う必要がなくなる。

- `mouse_cursor_set_mode(mode)` — カーソルモード設定
  - `MOUSE_CURSOR_NONE` (0): カーソル非表示 (生ポーリング専用)
  - `MOUSE_CURSOR_TEXT` (1): TVRAM属性反転カーソル
  - `MOUSE_CURSOR_GFX` (2): GFXスプライトカーソル (将来用)
- `mouse_cursor_show()` — カーソル表示
- `mouse_cursor_hide()` — カーソル非表示 (画面更新前にhide→更新→showのパターンで使用)

---

## §5-10 V86 サブシステムAPI

v34-v38で追加。PC-98ネイティブソフトウェア (ゲーム等) をV86モードで実行するAPI群。

> **注記 (2026-05)**: `sys_v86_boot_freedos` は廃止予定 (Phase F)。
> 新規コードでは `sys_v86_boot_image` を使用してください。

- `sys_v86_boot_image(path, cmdline)` — ディスクイメージ (.d88/.fdi/.hdi/.img) からV86ブート **(推奨)**
- `sys_v86_boot_native(path, cmdline)` — ネイティブモードでV86ブート (boot_image に統合予定)
- `sys_v86_boot_physical(drv, cmdline)` — 実FDDからV86ブート
- `sys_v86_boot_physical_ex(drv, media, cmdline)` — 実FDD + メディア種別指定
- `sys_v86_boot_freedos(path, cmdline)` — ⚠️ **廃止予定** (DOS専用パス)
- `sys_v86_set_debug(enabled)` — V86デバッグログ有効/無効

**終了方法**: ゲスト環境で Ctrl+GRPH+DEL

---

## §5-11 ループデバイス・ブロックI/O API

v37-v38で追加。ディスクイメージファイルをブロックデバイスとしてアタッチし、LBAベースの読み書きを提供する。

- `loop_attach(path, slot)` — イメージファイルをループデバイススロットにアタッチ
- `loop_detach(slot)` — ループデバイスをデタッチ
- `loop_status(slot, *total, *bps)` — ループデバイスの状態取得
- `dev_blk_read(dev_name, lba, count, buf)` — デバイス名指定のLBAブロック読み込み
- `dev_blk_write(dev_name, lba, count, buf)` — デバイス名指定のLBAブロック書き込み

**対応フォーマット**: D88, FDI, HDI, RAW (.img)

---

## §5-12 DB (SQLite) API

v29-v31で追加。カーネル内蔵SQLiteへのアクセスインターフェース。

- `db_open(path)` — データベースファイルをオープン (ハンドル返却)
- `db_close(handle)` — ハンドルクローズ
- `db_exec(handle, sql)` — SQL文を直接実行 (結果不要の場合)
- `db_prepare(handle, sql)` — プリペアドステートメント作成
- `db_step(handle)` — ステートメントの次の行を取得
- `db_column_int(handle, col)` — 整数カラム値取得
- `db_column_text(handle, col)` — テキストカラム値取得
- `db_finalize(handle)` — ステートメント解放
- `db_last_error(handle)` — 最後のエラーメッセージ取得
- `db_mem_used()` — SQLiteメモリ使用量取得

**制約**: 同時オープンは最大4ハンドル。VFS経由でファイルにアクセスするため、マウント済みのパスのみ使用可能。

---

*Last Updated: 2026-05-11*
