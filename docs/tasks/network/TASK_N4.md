# TASK_N4 — GUI から Host Services を使う (libos32gui 末尾追記 + ファイラ印刷 + 端末コピペ)

発行: PM (2026-09-14) / 状態: **実装へ (第 4 版で確定。Fable 設計レビュー 3 往復 Approve、blocker 0)。N4a = 基盤 (Claude Code PM)、N4b = アプリ層 (別エージェント)。実装中に §5 の残件を票へ反映**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §5、libos32host は [TASK_N3.md](TASK_N3.md) (受入済み)。KAPI 不変 (v51)。
**分担 (ROLES §0)**: §1 (N4a、基盤 = libos32gui shlib への host_* 末尾追記) = Claude Code PM。§2 (N4b、アプリ層 = ファイラ「印刷」・端末コピペ) = 別エージェント (Claude Code は設計 + レビュー)。

## 0. 前提
GUI アプリは CPL=3 で `KernelAPI` を受け取り、shlib (libos32gui、`.text` 共有) を呼ぶ。cfg は S2 で shlib のジャンプ表 101〜104 に `os32gui_cfg_*` を足した前例がある (1 呼び出しで完結、`app:` scope 限定、OS 側が open〜close を閉じる)。host_* は libos32host (N3、C 静的) が AGAIN ループ・宣言長・ストリーミングを既に隠すので、**shlib は libos32host.a をリンクして薄く公開するだけ**でよい。GUI での待ち (`sys_yield`) は park になり WM が回る (K7/T8 の作法)。**ただしスロット持ちの GUI アプリが host_* の AGAIN で park するのは本票が初** (T9 までの WAIT_POLL は CUI/GFX 子だけ) — park 中も IRQ0 の `link_tick` が進めるので進捗は止まらないが、印刷/取得中はそのアプリの UI が固まり CPU を使う。受入で「印刷中に別窓が動く / present が続く」を見る (往復 2 新 2)。GUI 内クリップボードは無い (`clip.rs` は描画クリップ) ので、コピペは**ホストのクリップボード**を直に使う。

## 1. N4a — libos32gui の host_* 公開 (基盤、Claude Code PM)
shlib のジャンプ表に host_* を末尾追記。**cfg は 101〜104 (4 本)、現在 `nfunc = SHLIB_NFUNC = 105`、次の空きは 105** (往復 1 B1: 114 は誤り)。host_* は **105〜110**、追記後 `nfunc` / `SHLIB_NFUNC` = **111**。関数 (libos32host を呼ぶ薄い extern "C"):

| 表 | 関数 | 動作 |
|---|---|---|
| 105 | `os32gui_host_get(url_ptr, url_len, out, cap, http_status)` → i32 | url は ptr+len で受け private NUL バッファ (`HOST_REQ_MAX−4`) へ写す。`host_get` を **out (cap)** に受ける。**戻り = 受信実長 (snprintf 流)、`out` には `min(実長, cap)` を写す** (呼び手は `ret > cap` で切れたと判る、B3)。本文はバイナリ可なので UTF-8 境界処理はしない。`http_status` は NULL 可 |
| 106 | `os32gui_print_text(name_ptr, name_len, buf, len, pages, svc_status)` → i32 | `host_print_text`。`pages` / `svc_status` (409/500/503 の業務値) は NULL 可 |
| 107 | `os32gui_print_file(name_ptr, name_len, path_ptr, path_len, pages, svc_status)` → i32 | path (ptr+len → 256B) を開いて `host_print_stream` (メモリ一定)。ファイル I/O は TDD で差し替えられる継ぎ目 (`hostsvc_file_*`、本体は `os32api::api()`、fake は host_tests)。FD は全経路 close。open 失敗の畳み: 引数不正 / ディレクトリ = `HOST_EINVAL`、その他の open/read 失敗 = `HOST_EIO` (新 8)。`src < 0` は `HOST_EABORT` で CLOSE を送らず戻る (job 放置は N3 仕様)。name の制御文字は落とす |
| 108 | `os32gui_clip_get(out, cap, total, svc_status)` → i32 | `host_clip_get`。**戻り = `out` に書いた長さ** (UTF-8 境界まで戻した後、= 呼び手が注入してよいバイト数)、`total` (NULL 可) = 受信実長 (呼び手は `*total > ret` で切れたと判る、往復 2 新 1)。境界は表 20 `os32gui_utf8_truncate`。`svc_status` (NULL 可、backend 無し = 503)。`cap == 0` / `out == NULL` → `HOST_EINVAL`。Rust stub 形 `clip_get(out) -> GuiResult<(written, total)>` |
| 109 | `os32gui_clip_put(buf, len, svc_status)` → i32 | `host_clip_put` (1〜4096、超過 `HOST_EINVAL`)。`svc_status` NULL 可 |
| 110 | `os32gui_host_time(out20)` → i32 | `host_time` (out は 20B) |

- 実装は `userland/rust/libos32gui/src/hostsvc.rs` (cfgro.rs と同流儀) に extern "C" ラッパー、`shlib.rs` の表に登録、C 実体は libos32host.a。エラーは `HOST_E*` (−100〜−106) をそのまま i32 で返す (GUI 側が文言化、`OS32_ERR_*` ではない)。ポインタ+長さは呼び出し前に検証。
- **`kapi` は cfgro.rs の `#[no_mangle] static mut kapi` を共用する。hostsvc.rs で `kapi` を再定義しない** (shlib リンクは `--allow-multiple-definition` なので重複が黙ってエラーにならず libos32host が NULL を読む事故になる)。各 wrapper は `cfgro::kapi_ready()` の門を通す。
- `sdk/rust/os32api/src/host.rs` に libos32host 6 本の `extern "C"` 宣言 + `HOST_E*` を「宣言 1 か所」で置き、stub (`libos32gui_stub`) は**常に必須の束縛**として同シグネチャを持つ (試験専用ではない)。const assert で `os32api::host` と写しを照合 (cfgro と同じ)。
- ビルド: `build/programs.mk` の shlib リンクに `libos32host.a` を足す (`--allow-multiple-definition` 既存)。`--api` を **51** に上げる (host_* = `kapi.json` の api 配列 **208〜212** = `kapi_generated.rs` struct idx 210〜214)。`libos32host.o` は bss 16KB (`g_stream_buf`) なので shlib の per-app .bss が +4 ページ (4→8 ページ) → `MEMORY_BUDGET.md` に記録。受入で `nm userland/libos32gui.elf | grep -c " [BDbd] kapi$"` == 1 (kapi 二重定義防止)。**新 shlib は `/sys/lib` なので配備は `make deploy` → `hsync sys`** (既定 hsync は /sys を飛ばす、新 9)。
- ホスト試験: `userland/rust/libos32gui/host_tests/` に host_* ラッパーの分岐 (ポインタ検証・cap 超過・エラー透過) を贋 libos32host で。

## 2. N4b — ファイラ印刷 + 端末コピペ (アプリ層、別エージェント)
- **ファイラ「印刷」** (`userland/rust/filer/`): 選択中のファイルを `os32gui_print_file(basename, path, &pages)`。結果 (pages / エラー) をモーダルかステータスに表示。メニュー/キーに項目を足す。大きいファイルでも print_file がストリーミングするのでメモリ一定。
- **端末コピー/貼り付け** (`userland/rust/t5a_display/` + `libos32term`): 端末に**選択 UI は無い** (往復 1 B4) ので v1 を縮める:
  - **貼り付け** (高価値、選択不要): `os32gui_clip_get(buf, cap, &total, &svc)` の**書いた長さ**を端末の保留貼り付けバッファ (≤4096) に取り、既存の 100ms タイマ (`guest.rs` の `on_timer`) ごとに `min(残り, 256 − kbd_inject_pending())` だけ `kbd_inject` し戻り値ぶん進める (`KBD_INJECT_RING_SIZE 256`、B2)。**drip の `rc < len` は既存 `inject()` の `inject_short` (消えた打鍵) に計上しない** (専用経路、新 5)。`reader == false` の busy 端末では捨てる、子が終わって `Mode::Prompt` に戻ったら保留を捨てる。attach モードは LF → 0x0D。**`Mode::Prompt` では最初の行 (最初の LF まで) だけ `line` へ入れ残りは捨てる** (`prompt::Line::push` は制御バイトを落とすため、複数行はプロンプトに入れない、新 6)。
  - **キー**: コピー / 貼り付けは `inject::from_key` が空を返すキー (ファンクションキー等、CTRL+英字は gshell が制御コードにするので不可) から選び票 §2 に具体名を書く (新 7)。
  - **コピー** (v1 は簡易): マウス範囲選択は作らず、**キーで「可視画面 (または直近 N 行) を丸ごとコピー」** = `libos32term::Model` の `viewport` からセルを UTF-8 に戻し (Wide は 1 文字・継続セルは飛ばす・行末空白は落とす・行は 0x0A)、`os32gui_clip_put`。4096B 超は UTF-8 境界で切る。**マウスによる範囲選択は N4b の後続 (別票)**。
  4096B 上限 (CLIP PUT v1)。
- 受入 (GUI): gshell 上でファイラから実ファイルを印刷しホストの spool に落ちる、端末で選択→コピー→別の場所で貼り付け、ホストのクリップボードと往復。`tools/gui_gate.py` 系で PM 観測。

## 3. レビューで見てほしい点
1. §1 の shlib 公開が cfg (101〜105) と同じ作法で安全か (ポインタ検証、エラー透過、GUI park との両立)。host_get の out バッファ版と sink 版の切り分け (GUI に大容量取得が要るか)。
2. libos32host.a を shlib にリンクすることの是非 (shlib の `.text` サイズ、`kapi` の渡り方 = cfgro.rs の `kapi` static と同じか)。
3. §2 の端末コピペがホストのクリップボードだけで完結し、GUI 内クリップボード不在で困らないか。ファイラ印刷の大容量ストリーミング。
4. 分担の境界: **アプリは host_* KAPI (210〜214) を呼ばず libos32host.a もリンクせず、stub の `os32gui_host_*` 6 本だけを使う** (filer/端末は既に sys_open/kbd_inject を KAPI 直呼びしているので「KAPI 直呼びしない」ではなく「host_* は shlib 経由」が境界)。B1〜B4 が直れば N4b は stub 6 本 + 端末の `kbd_inject` だけで閉じ別エージェントに渡せる。
5. B4 で v1 を「貼り付け + 画面コピー」に縮めた判断 (マウス範囲選択を別票に) の是非。
6. 受入手順: Agent を `--clip auto|wsl|file:<path>` + `--spool-dir` で起動 (既定 none は 503)、配備は `make deploy` → `hsync sys`。
7. clip_get の書いた長さ / total の契約 (新 1) と host_tests、drip の分割 (新 5/6)、park 中の WM 応答 (新 2)。

## 5. 実装時に反映する残件 (最終レビュー Approve の non-blocker)
**N4a (基盤)**:
- clip_get の切り詰めは **NUL 以降も捨てる** (`written < total` は cap だけでなく NUL でも起きる、呼び手はそう理解する)。`utf8_truncate` は NUL で止まる既存挙動でよい。
- `total` を数えるため、host_get/clip_get の sink は **cap 到達後も捨てながら受理し続けて実長を数える** (sink は 0 を返す、非 0 は EABORT)。
- host_tests の `#[path]` 取り込みのため、UTF-8 の 2 関数を os32api 非依存の小モジュールへ出し client.rs / hostsvc.rs で共用、`checked_slice` を pub に。
- **stub の 6 本のシグネチャ表**を §1 に置く (並走する N4b が実物を待たずに書ける): `host_get(url, out, http_status)->i32`、`print_text(name, buf, pages, svc)->i32`、`print_file(name, path, pages, svc)->i32`、`clip_get(out)->(written,total)`、`clip_put(buf, svc)->i32`、`host_time(out20)->i32` (Rust 形)。clip_get の out は **NUL 終端しない** (cfg_get_text と違う)。
- **子終了時の注入リング残り**: GUI 子の `exec_exit` で `kbd_inject_discard()` を呼ぶ小改修 (KAPI 不変、K レーン) を N4a に含める。含めない場合は「貼り付けの余りが次の子へ渡る」を既知制限として票に残す (打鍵でも起きる既存挙動)。
**N4b (アプリ層)**:
- コピペのキー候補は **F6〜F10** (WM が横取りせず `from_key` も空)。**gshell が F キーを `GUI_EV_KEY` でアプリへ配るかを N4b で先に確認**してから確定。
- Prompt 貼り付けの 1 行が `LINE_MAX` (160) 超は `Line::push` が丸ごと捨てる → 切るか通知。attach 貼り付けは TAB/LF 以外の制御バイト (ESC 等) を落とす (子の暴走防止)。コピーはセルの NUL / ESC を空白に置換してから `clip_put`。

## 6. N4a 実装レビュー (Fable、2026-09-14) → N4a-fix 着地済み (B1 修正、con_sink PASS、host_tests 59)
判定 Request changes、blocker 1 件。6 ラッパー・kapi 配線・ジャンプ表・stub・utf8core は設計どおり。
- **B1 (blocker)**: `exec/exec.c:865` の子終了注入破棄が「読み手以外の**あらゆる**アプリ退場」で発火し、無関係な GUI アプリが畳まれると貼り付け中のリングが空になり子が 256B 欠けた貼り付けを受ける (新規退行)。→ 条件を **`launch_child(con_sink_reader_get()) == id`** (退場したのが端末の子のときだけ破棄) に絞る (`launch_child` は不正 id で 0)。受入: `con_sink_host.c` の (9) 写しに (9a) を足し「無関係 ID で pending 減らない / 読み手の child で 0」。
- nb: (1) host_fake の本文を可変チャンクにし out_sink の「cap 超過後も数える」を 2 チャンクで固定、(2) con_sink_host / multiapp_impl_host に破棄条件の試験、(3) `copy_name` の空名・制御文字のみは `HOST_EINVAL` か lpr の "file" 既定に揃える (空白は Agent が許すので落とさない)、(4) url 1396B 超 / name・path 256B 超 / NULL の反例を host_tests に、(6) `MEMORY_BUDGET.md` に per-app .bss 4→8 ページ (+16KB/GUI アプリ) を記録。
