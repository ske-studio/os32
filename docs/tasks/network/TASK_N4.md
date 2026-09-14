# TASK_N4 — GUI から Host Services を使う (libos32gui 末尾追記 + ファイラ印刷 + 端末コピペ)

発行: PM (2026-09-14) / 状態: **設計 第 2 版 (往復 1 の blocker 4 件を反映: ジャンプ表を 105〜110 / nfunc 111 に、貼り付けは 256B の注入リングに分割、cap 切りは実長返し + UTF-8 境界、端末の選択 UI が無いので v1 を貼り付け + 画面コピーに縮小。往復 2 待ち)**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §5、libos32host は [TASK_N3.md](TASK_N3.md) (受入済み)。KAPI 不変 (v51)。
**分担 (ROLES §0)**: §1 (N4a、基盤 = libos32gui shlib への host_* 末尾追記) = Claude Code PM。§2 (N4b、アプリ層 = ファイラ「印刷」・端末コピペ) = 別エージェント (Claude Code は設計 + レビュー)。

## 0. 前提
GUI アプリは CPL=3 で `KernelAPI` を受け取り、shlib (libos32gui、`.text` 共有) を呼ぶ。cfg は S2 で shlib のジャンプ表 101〜105 に `os32gui_cfg_*` を足した前例がある (1 呼び出しで完結、`app:` scope 限定、OS 側が open〜close を閉じる)。host_* は libos32host (N3、C 静的) が AGAIN ループ・宣言長・ストリーミングを既に隠すので、**shlib は libos32host.a をリンクして薄く公開するだけ**でよい。GUI での待ち (`sys_yield`) は park になり WM が回る (K7/T8、N3 の CUI と同じ作法)。GUI 内クリップボードは無い (`clip.rs` は描画クリップ) ので、コピペは**ホストのクリップボード**を直に使う。

## 1. N4a — libos32gui の host_* 公開 (基盤、Claude Code PM)
shlib のジャンプ表に host_* を末尾追記。**cfg は 101〜104 (4 本)、現在 `nfunc = SHLIB_NFUNC = 105`、次の空きは 105** (往復 1 B1: 114 は誤り)。host_* は **105〜110**、追記後 `nfunc` / `SHLIB_NFUNC` = **111**。関数 (libos32host を呼ぶ薄い extern "C"):

| 表 | 関数 | 動作 |
|---|---|---|
| 105 | `os32gui_host_get(url_ptr, url_len, out, cap, http_status)` → i32 | url は ptr+len で受け private NUL バッファ (`HOST_REQ_MAX−4`) へ写す。`host_get` を **out (cap)** に受ける。**戻り = 受信実長 (snprintf 流)、`out` には `min(実長, cap)` を写す** (呼び手は `ret > cap` で切れたと判る、B3)。本文はバイナリ可なので UTF-8 境界処理はしない。`http_status` は NULL 可 |
| 106 | `os32gui_print_text(name_ptr, name_len, buf, len, pages, svc_status)` → i32 | `host_print_text`。`pages` / `svc_status` (409/500/503 の業務値) は NULL 可 |
| 107 | `os32gui_print_file(name_ptr, name_len, path_ptr, path_len, pages, svc_status)` → i32 | path (ptr+len → 256B) を開いて `host_print_stream` (メモリ一定)。ファイル I/O は TDD で差し替えられる継ぎ目 (`hostsvc_file_*`、本体は `os32api::api()`、fake は host_tests)。FD は全経路 close、`vfs_open` のディレクトリ拒否は `HOST_EINVAL` に畳む |
| 108 | `os32gui_clip_get(out, cap)` → i32 | `host_clip_get`。**戻り = 実長、`out` は `min(実長, cap)` かつ UTF-8 境界まで戻す** (shlib の `os32gui_utf8_truncate` = 表 20 を使う、B3)。backend 無しは `HOST_ESERVICE` (503) |
| 109 | `os32gui_clip_put(buf, len, svc_status)` → i32 | `host_clip_put` (1〜4096、超過 `HOST_EINVAL`)。`svc_status` NULL 可 |
| 110 | `os32gui_host_time(out20)` → i32 | `host_time` (out は 20B) |

- 実装は `userland/rust/libos32gui/src/hostsvc.rs` (cfgro.rs と同流儀) に extern "C" ラッパー、`shlib.rs` の表に登録、C 実体は libos32host.a。エラーは `HOST_E*` (−100〜−106) をそのまま i32 で返す (GUI 側が文言化、`OS32_ERR_*` ではない)。ポインタ+長さは呼び出し前に検証。
- **`kapi` は cfgro.rs の `#[no_mangle] static mut kapi` を共用する。hostsvc.rs で `kapi` を再定義しない** (shlib リンクは `--allow-multiple-definition` なので重複が黙ってエラーにならず libos32host が NULL を読む事故になる)。各 wrapper は `cfgro::kapi_ready()` の門を通す。
- `sdk/rust/os32api/src/host.rs` に libos32host 6 本の `extern "C"` 宣言 + `HOST_E*` を「宣言 1 か所」で置き、stub (`libos32gui_stub`) は**常に必須の束縛**として同シグネチャを持つ (試験専用ではない)。const assert で `os32api::host` と写しを照合 (cfgro と同じ)。
- ビルド: `build/programs.mk` の shlib リンクに `libos32host.a` を足す (`--allow-multiple-definition` 既存)。`--api` を **51** に上げる (host_* KAPI 210〜214)。`libos32host.o` は bss 16KB (`g_stream_buf`) なので shlib の per-app .bss が +4 ページ → `MEMORY_BUDGET.md` に記録。
- ホスト試験: `userland/rust/libos32gui/host_tests/` に host_* ラッパーの分岐 (ポインタ検証・cap 超過・エラー透過) を贋 libos32host で。

## 2. N4b — ファイラ印刷 + 端末コピペ (アプリ層、別エージェント)
- **ファイラ「印刷」** (`userland/rust/filer/`): 選択中のファイルを `os32gui_print_file(basename, path, &pages)`。結果 (pages / エラー) をモーダルかステータスに表示。メニュー/キーに項目を足す。大きいファイルでも print_file がストリーミングするのでメモリ一定。
- **端末コピー/貼り付け** (`userland/rust/t5a_display/` + `libos32term`): 端末に**選択 UI は無い** (往復 1 B4) ので v1 を縮める:
  - **貼り付け** (高価値、選択不要): `os32gui_clip_get(buf, cap)` の内容を端末の入力へ。**注入リングは 256B で溢れは捨てる** (`KBD_INJECT_RING_SIZE`、B2) ので、端末に保留貼り付けバッファ (≤4096) を持ち、既存の 100ms タイマ (`guest.rs` の `on_timer`) ごとに `min(残り, 256 − kbd_inject_pending())` だけ `kbd_inject` し戻り値ぶん進める。`Mode::Prompt` 中は `line` へ足す。LF → 0x0D (シェルの行入力は 0x0D のみ行末)。
  - **コピー** (v1 は簡易): マウス範囲選択は作らず、**キーで「可視画面 (または直近 N 行) を丸ごとコピー」** = `libos32term::Model` の `viewport` からセルを UTF-8 に戻し (Wide は 1 文字・継続セルは飛ばす・行末空白は落とす・行は 0x0A)、`os32gui_clip_put`。4096B 超は UTF-8 境界で切る。**マウスによる範囲選択は N4b の後続 (別票)**。
  4096B 上限 (CLIP PUT v1)。
- 受入 (GUI): gshell 上でファイラから実ファイルを印刷しホストの spool に落ちる、端末で選択→コピー→別の場所で貼り付け、ホストのクリップボードと往復。`tools/gui_gate.py` 系で PM 観測。

## 3. レビューで見てほしい点
1. §1 の shlib 公開が cfg (101〜105) と同じ作法で安全か (ポインタ検証、エラー透過、GUI park との両立)。host_get の out バッファ版と sink 版の切り分け (GUI に大容量取得が要るか)。
2. libos32host.a を shlib にリンクすることの是非 (shlib の `.text` サイズ、`kapi` の渡り方 = cfgro.rs の `kapi` static と同じか)。
3. §2 の端末コピペがホストのクリップボードだけで完結し、GUI 内クリップボード不在で困らないか。ファイラ印刷の大容量ストリーミング。
4. 分担の境界: **アプリは host_* KAPI (210〜214) を呼ばず libos32host.a もリンクせず、stub の `os32gui_host_*` 6 本だけを使う** (filer/端末は既に sys_open/kbd_inject を KAPI 直呼びしているので「KAPI 直呼びしない」ではなく「host_* は shlib 経由」が境界)。B1〜B4 が直れば N4b は stub 6 本 + 端末の `kbd_inject` だけで閉じ別エージェントに渡せる。
5. B4 で v1 を「貼り付け + 画面コピー」に縮めた判断 (マウス範囲選択を別票に) の是非。
6. 受入手順: Agent を `--clip auto|wsl|file:<path>` + `--spool-dir` で起動 (既定 none は 503)。
