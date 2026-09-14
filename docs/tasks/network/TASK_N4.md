# TASK_N4 — GUI から Host Services を使う (libos32gui 末尾追記 + ファイラ印刷 + 端末コピペ)

発行: PM (2026-09-14) / 状態: **設計 第 1 版 (Fable 設計レビュー待ち)**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §5、libos32host は [TASK_N3.md](TASK_N3.md) (受入済み)。KAPI 不変 (v51)。
**分担 (ROLES §0)**: §1 (N4a、基盤 = libos32gui shlib への host_* 末尾追記) = Claude Code PM。§2 (N4b、アプリ層 = ファイラ「印刷」・端末コピペ) = 別エージェント (Claude Code は設計 + レビュー)。

## 0. 前提
GUI アプリは CPL=3 で `KernelAPI` を受け取り、shlib (libos32gui、`.text` 共有) を呼ぶ。cfg は S2 で shlib のジャンプ表 101〜105 に `os32gui_cfg_*` を足した前例がある (1 呼び出しで完結、`app:` scope 限定、OS 側が open〜close を閉じる)。host_* は libos32host (N3、C 静的) が AGAIN ループ・宣言長・ストリーミングを既に隠すので、**shlib は libos32host.a をリンクして薄く公開するだけ**でよい。GUI での待ち (`sys_yield`) は park になり WM が回る (K7/T8、N3 の CUI と同じ作法)。GUI 内クリップボードは無い (`clip.rs` は描画クリップ) ので、コピペは**ホストのクリップボード**を直に使う。

## 1. N4a — libos32gui の host_* 公開 (基盤、Claude Code PM)
shlib のジャンプ表に host_* を末尾追記 (cfg の 101〜105 の後、現在の最大 113 の後 = 114〜)。関数 (libos32host を呼ぶ薄い extern "C"):

| 表 | 関数 | 動作 |
|---|---|---|
| 114 | `os32gui_host_get(url, out, cap, http_status)` → i32 | `host_get` を **out バッファ (cap)** に受ける版 (GUI は小さい取得が主。cap 超過は切って返り値 = 実長 or 負)。大容量は N4b が要れば sink 版を別途 |
| 115 | `os32gui_print_text(name, buf, len, pages)` → i32 | `host_print_text` |
| 116 | `os32gui_print_file(name, path, pages)` → i32 | path を開いて `host_print_stream` (ファイラ印刷用、メモリ一定) |
| 117 | `os32gui_clip_get(out, cap)` → i32 | `host_clip_get` を out に (返り値 = 実長 / 負) |
| 118 | `os32gui_clip_put(buf, len)` → i32 | `host_clip_put` |
| 119 | `os32gui_host_time(out20)` → i32 | `host_time` |

- 実装は `userland/rust/libos32gui/src/hostsvc.rs` (cfgro.rs と同流儀) に extern "C" ラッパー、`shlib.rs` の表に登録、C 実体は libos32host.a。エラーは `HOST_E*` をそのまま i32 で返す (GUI 側が文言化)。ポインタ+長さは呼び出し前に検証 (cfgro.rs と同じ)。
- ビルド: `build/programs.mk` の shlib リンクに `libos32host.a` を足す。`--api` は shlib の版に合わせる。stub (`libos32gui_stub`) にも同じシグネチャを足す (ホスト試験用)。
- ホスト試験: `userland/rust/libos32gui/host_tests/` に host_* ラッパーの分岐 (ポインタ検証・cap 超過・エラー透過) を贋 libos32host で。

## 2. N4b — ファイラ印刷 + 端末コピペ (アプリ層、別エージェント)
- **ファイラ「印刷」** (`userland/rust/filer/`): 選択中のファイルを `os32gui_print_file(basename, path, &pages)`。結果 (pages / エラー) をモーダルかステータスに表示。メニュー/キーに項目を足す。大きいファイルでも print_file がストリーミングするのでメモリ一定。
- **端末コピー/貼り付け** (`userland/libos32term/` + 端末アプリ): 選択範囲のテキストを `os32gui_clip_put(sel, len)` (コピー)、`os32gui_clip_get(buf, cap)` の内容を端末の入力へ流す (貼り付け)。範囲選択の UI は端末側の既存の仕組みに接続。4096B 上限 (CLIP PUT v1)。
- 受入 (GUI): gshell 上でファイラから実ファイルを印刷しホストの spool に落ちる、端末で選択→コピー→別の場所で貼り付け、ホストのクリップボードと往復。`tools/gui_gate.py` 系で PM 観測。

## 3. レビューで見てほしい点
1. §1 の shlib 公開が cfg (101〜105) と同じ作法で安全か (ポインタ検証、エラー透過、GUI park との両立)。host_get の out バッファ版と sink 版の切り分け (GUI に大容量取得が要るか)。
2. libos32host.a を shlib にリンクすることの是非 (shlib の `.text` サイズ、`kapi` の渡り方 = cfgro.rs の `kapi` static と同じか)。
3. §2 の端末コピペがホストのクリップボードだけで完結し、GUI 内クリップボード不在で困らないか。ファイラ印刷の大容量ストリーミング。
4. 分担の境界 (N4a 基盤 / N4b アプリ) が公開ラッパーだけで結べているか (アプリが KAPI 直呼びやカーネルを触らないか)。
