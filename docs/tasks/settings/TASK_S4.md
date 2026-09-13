# S4 — gshell が設定レジストリを読む最初の消費者 + 設定ダイアログ

状態: **設計 第 3 版 (往復 2 の 2 件 + non-blocker を反映、往復 3/3 = 最終待ち)**。前提: S2 完了 (`libos32cfg` = `userland/lib/cfg/libos32cfg.h`、KAPI v50、`cfg` コマンド、libos32gui の `os32gui_cfg_*` 4 本、main `e09c458`)。決裁: [S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §3-4 (最初の消費者は数キー)、§4 (設定の読み書きは OS 経由だけ、接続は同時 1 本、open〜close の間に yield しない)。
契約の正典: [S0_FOUNDATION.md](S0_FOUNDATION.md) §2、[DESIGN.md](DESIGN.md) §3〜§5、[TASK_S2.md](TASK_S2.md) §1 (API と規則 1〜8)。gshell の契約は docs/tasks/gui/ (S6 = CUI 復帰、S8 = handler の禁止事項、X4 = 描画 callback)。
規約: gshell は Rust (no_std、`os32api` のみ依存)、[C1] は C 側に、[V2] deploy.yaml、コーダーは worktree + ホスト TDD のみ。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S4-W** | gshell の起動時読み込み (`gshell` scope の 2 キー)、Start メニュー「Settings...」、WM 内蔵の設定ダイアログ (読み → 変更 → OK で書き戻し → 即時反映)、ホスト TDD | W | `userland/gshell/src/{settings.rs (新規), lib.rs, startmenu.rs, desktop.rs, taskbar.rs, modal.rs, ffi.rs}`、`sdk/rust/os32api/src/cfg.rs` (新規: `cfg_*` の extern "C" 宣言を 1 か所に。libos32gui の `cfgro.rs` もこれを使うよう寄せる)、`userland/gshell/host/*` |
| 共有 (PM) | `build/programs.mk` (gshell.elf のリンクに `$(LIBCFG_OBJ)`)、**`build/app.conf` (gshell の要求 KAPI 49 → 50**、往復 1 の B7: v49 カーネルが受理して v50 スロットを呼ぶ経路を版検査で塞ぐ)、`assets/settings/defaults.tsv` (`desktop/color` の既定を 12 に = 現行 `GUI_COLOR_DESKTOP`、`ebe4573` で済)、`tools/gui_gate.py` (`START_MENU_ITEMS` 5 → 6 **と項目名の定数化** `ROW_PROGRAMS/FILEMAN/RUN/SETTINGS/CUI/HALT`、`leave_gshell` の行 3 → `ROW_CUI` = 4、halt 経路の行 4 → `ROW_HALT` = 5、B5)、`tools/mk_settings_db.py` (`--schema-version N`、試験 fixture 用)、`userland/deploy.yaml` (fixture `/etc/settings.v2.fixture`、tags test)、`tools/emu_agent/agent.py`、`docs/tasks/gui/` の Start メニュー記述 (行番号。過去の受入記録は当時の 5 項目のまま残し「適用版」を注記) | PM | — |

S4 に**含めない**: 壁紙の描画 (`desktop/wallpaper` は S4 では読まない・出さない。画像の読み込み経路 (MGX) と 386 の再描画コストの実測を S5 で見てから v1.4 で)、窓位置の記憶、`assets/filetypes` の DB 移行 (次段)、他アプリへの変更通知 (v1.x は「自分で読み直す」、DESIGN §7)。

## 1. 読むキーと意味 (`gshell` scope)

| key | 型 | 意味 | 範囲外 / 欠損 | 既定 (defaults.tsv) |
|---|---|---|---|---|
| `desktop/color` | int | デスクトップ背景のシステム色 index (0〜15、`GUI_COLOR_*` と同じ 16 色) | 0〜15 以外・NOTFOUND → `GUI_COLOR_DESKTOP` (12) | **12** (S0-T の `1` は誤りなので改める。既定で見た目が変わらない) |
| `taskbar/clock_24h` | int | 1 = `HH:MM` (現行)、0 = `h:MM AM` / `h:MM PM` (12 時間、時は空白詰めなし) | 0 / 1 以外・NOTFOUND → 1 | 1 |

- 読むのは **起動時 1 回** (`main` → gfx attach の後、最初の合成の前) と、**設定ダイアログを開いたとき** (現在値の表示のため)。X4 / IRQ / 描画 callback / handler (S8) では DB に触らない。
- `desktop/color` の反映: `desktop::fill` が `GUI_COLOR_DESKTOP` 定数の代わりに `GuiState.cfg.desktop_color` (u8) を使う。2 色モード (lease、市松) は従来どおり色を使わない。
- `clock_24h` の反映: `taskbar::tick_clock` の整形が `GuiState.cfg.clock_24h` を見る。12 時間表記は最長 8 文字 (`12:59 PM`) なので `clock_rect` の幅を **文字数から** 決める (`w = 文字数 × 8 + 8`、右詰め)。**幅が変わる更新 (`12:59 PM` → `1:00 PM`、24h ↔ 12h の切替) は旧矩形と新矩形の和集合を dirty / present の対象にし、窓ボタン帯の可視数が変わるならその帯も描き直す** (往復 1 の B4: 現行 `tick_clock` は新矩形だけを dirty にするので旧左端 8px が残る)。`tools/gui_gate.py` の時計座標 (614, H-12) は 12h でも矩形内 (640px 画面)。

## 2. 起動時の読み込み (`settings.rs`)

```rust
pub struct GuiCfg {
    pub desktop_color: u8, pub clock_24h: bool,       /* 画面に**適用済み**の値 */
    pub status: i32,        /* CFG_* / -1 = cfg_open が負 */
    pub schema_version: i32,/* cfg_schema_version の実値 (VERSION の表示用) */
    pub sqlite: i32,        /* cfg_last_sqlite */
    pub close_error: i32,   /* 直近の close 失敗 (0 = 無し)。**再 load が成功しても消さない** (利用者が見るまで残す。往復 1 non-blocker) */
    pub load_ticks: u32, pub save_ticks: u32,  /* 計測 (G7)。ダイアログの状態行に出す */
}
pub fn load(prev_close_error: i32) -> GuiCfg   /* cfg_open(&db, 0) → cfg_get_int ×2 → cfg_close。全経路で既定値を埋める */
```
- 3 種の値を区別する (往復 1 non-blocker): **適用値** (`GuiState.cfg`、画面が使う)、**再読込値** (ダイアログを開いたときの `load()` の結果 = 編集の起点)、**編集中の値** (モーダル私有)。OK の「変更なし」判定は編集中の値と再読込値の比較、画面への反映は保存成功後に適用値へ写す。
- `cfg_open` が負 (引数 / メモリ) → 既定値、`status = -1`。`cfg_status` が MISSING / CORRUPT / VERSION / ERROR でも **起動は止めない** (既定値で動く。DESIGN §5 の fallback)。`VERSION` は読める値をそのまま使う。
- `cfg_close` の失敗は **握りつぶさない**: `GuiCfg.close_error` に残し、起動時通知と設定ダイアログの状態行に出す (`cfg_last_close_error` は rollback 失敗も含むので文言は `close failed (<code>)` とし「隔離 slot」とは断定しない)。起動の可否には影響しない。
- **起動時通知** (DESIGN §5、往復 1 の B3): `status` が OK 以外 (MISSING / CORRUPT / VERSION / ERROR / open 負) か `close_error != 0` なら、**最初の合成の後、top-level の最初の周回**で WM メッセージ (`open_wm_message`、OK のみ、purpose `PURPOSE_CFG_NOTICE`) を 1 回出す: `Settings: MISSING - defaults in use. Run 'cfg init' in CUI.` / `Settings: CORRUPT - defaults in use` / `Settings: VERSION <n> (read only)` / `Settings: ERROR sqlite=<code> - defaults in use` / `Settings: close failed (<code>)`。通知はモーダルなので利用者が OK を押すまで他の操作を待たせる (起動直後の 1 回だけ)。
- 所要時間: `load()` と保存の前後で `tick_count` を読み `load_ticks` / `save_ticks` に持つ (S5 の材料)。**採取経路** (往復 1 の B3: gshell 実行中の `kprintf` は con_sink に入り CUI 画面には残らない): (1) 設定ダイアログの状態行に `load <n>t save <n>t` を出す (スクリーンショットで読める)、(2) `kprintf` の 1 行 `gshell: cfg <status> color=<n> clock24=<0/1> load=<n>t` は con_sink に入るので **端末アプリ (`t5a_display`) を開けば `prev_runs` として読める**。受入 G1 / G3 / G7 はこの 2 経路で採る。

## 3. 設定ダイアログ (WM 内蔵モーダル、`modal.rs` の新 purpose `PURPOSE_SETTINGS`)

- 入口: Start メニューの root に **「Settings...」を Run... の次 (index 3) に追加** (`IT_SETTINGS`、`ROOT_ITEMS` 5 → 6、CUI mode / Shut Down は 1 行下がる)。`tools/gui_gate.py` の `START_MENU_ITEMS` を 6 に (PM)。既存の台本 (`leave_gshell` は行 3 → **行 4**) は `start_row` から導いているので定数の更新だけで追従する。
- 見た目 (既存モーダルの部品だけで組む。新しいウィジェットは作らない): タイトル `Settings`、本文 2 行 (list band の row 0 / 1 を流用)、下にボタン `OK` / `Cancel`。**16 色リース中 (契約 G8、`lease::mono`) は既存 `draw_list` と同じ 2 色分岐**: 選択行は `TEXT` / `WINDOW` の反転、色見本は描かず数値だけ (`Desktop color : 5 (preview off: palette leased)`)、ボタン・枠も 2 色 (往復 2 の R2: WM モーダルは前面アプリを入れ替えないので借り手が変えた色が残る)。リース中の Open / 操作 / OK 後の再描画をホスト試験に含める。
  - row 0: `Desktop color : <n>  [<色の 16px 見本>]`、row 1: `Clock : 24h` / `Clock : 12h`。
  - 選択行は SEL_BG で反転。↑ / ↓ で行移動、← / → / SPACE で値を変える (color は 0..15 を循環、clock はトグル)、RETURN = OK、ESC = Cancel。マウスは row クリックで選択 + 同じ row の再クリックで値を進める、ボタンはクリック。**入力欄 (テキスト) は使わない** (数値入力の検証を持ち込まない)。
  - 状態行 (本文 3 行目、小さく): `settings.db: OK` / `MISSING - run 'cfg init' in CUI` / `CORRUPT` / `VERSION <n> (read only)` / `ERROR sqlite=<code>` / `close failed (<code>)`。
- **DB に触るのは top-level (owner 1) だけ** (往復 1 の B1): Start → Settings... は WM の入力配送 (アプリ 1 本が `OP_WAIT` 中なら `handler` → `wm_cycle(Ctx::Wait)` の文脈) で起きるので、そこでは `load()` を呼ばず **予約** `settings::req = Some(Req::Open)` を立てるだけ。`multiapp::should_park` の (a) と `pick_poll` の門に `settings::pending()` を足し (T9 の `launch_work_pending` と同じ扱い)、`standalone_loop` の `launch_pending` の隣で消費する: `Req::Open` → `load()` → 値を持ってモーダルを開く (この時点で DB は閉じている)。`Req::Save{color, clock}` → 保存 → 結果で適用 / 通知。**モーダルの OK は `finish_wm` で `Req::Save` を予約して閉じるだけ** (DB に触らない)。予約は 1 本 (`Option`)、二重予約は後勝ちにしない (Open 中に Save は来ない: モーダルが 1 つだから)。
- **モーダル枠の競合 (往復 2 の R1)**: 予約から消費までの間にアプリが走り自分のモーダル (X3 `MODAL_OPEN`) を開くことがある (`op_wait` は `wm_cycle` の後に `wake_ready` でアプリへ戻れ、`should_park` の判定は次の周回の先頭)。WM のモーダル枠は 1 つで `open_wm` は既存モーダルがあると拒否するので、**開けなかったものは捨てず保持して再試行する**契約にする:
  - `Req::Open` は `modal::is_open()` が偽の周回でだけ消費する (真なら予約を残す。予約が残る限り `should_park` / `pick_poll` の門は真のままなので top-level に毎周回戻り、アプリのモーダルが閉じた次の周回で開く)。`load()` は開ける周回で呼ぶ (古い値を持ち越さない)。
  - `Req::Save` は DB 操作をモーダル枠と無関係に**その周回で実行**する (適用値の更新と画面反映も同じ周回)。結果の通知だけは `settings::notice = Some(Notice)` に置き、`modal::is_open()` が偽になった周回で `open_wm_message` を出す (起動時通知も同じ `notice` 経路)。`notice` は 1 本、後から来た通知が先のものを上書きしない (先のが出るまで次は `Req` 側に留める。実際には Save は Settings を閉じた後にしか起きず、Settings は通知が出るまで開けないので同時に 2 本にはならない — これを試験で固定する)。
  - `open_wm_message` の戻り値 (`open_wm` の bool) を通知経路では**必ず見る**。偽なら `notice` を保持したまま次の周回へ。
  - アプリ側の `MODAL_OPEN` は拒否しない (アプリ向けの契約を変えない)。
- 開いたとき (top-level で `Req::Open` を消費した地点): `load()` を**もう一度**呼んで現在値を出す (起動後に `cfg` コマンドで変わった値を反映する。DESIGN §7「自分で読み直す」)。読めなければ既定値を出し、状態行に理由。
- **OK** (書き戻し点。決裁「設定が変わったときだけ」): `Req::Save` は **変更マスク (color / clock のどれが変わったか) と再読込値と編集値**を `settings` モジュールの私有状態に持つ (モーダルが閉じても失われない。往復 2 non-blocker)。`save_ticks` は再 Open の `load()` で消さない。判定順は (1) 編集中の値が再読込値と**同じなら何もしない** (予約も open もしない、`gshell: cfg write skipped` を kprintf)、(2) 再読込が OK でなかった (MISSING / CORRUPT / VERSION / ERROR / open 負) なら **保存せず**メッセージ (`cannot save: <status>`)、(3) それ以外は `Req::Save` を予約して閉じる。top-level で `Req::Save` を消費するとき **1 つの周回の中で** `cfg_open(&db, 1)` → `cfg_begin` → `cfg_set_int` (変わったキーだけ) → `cfg_commit` → `cfg_close` を完結させる (間に yield / 合成 / 他イベント処理を挟まない)。結果の扱い (往復 1 の B2):
  - **保存失敗** (`cfg_open` 負、status が OK でない、begin / set / commit の負): メッセージ (`save failed: <段> (<code>)`) を出し、**適用値は変えない** (rollback は `cfg_close` が行う。close の戻りは `close_error` へ)。
  - **保存成功・後始末失敗** (commit は成功、`cfg_close` が負): commit 済みの値が正なので **適用値を更新し**画面に反映したうえで、別のメッセージ `saved, but close failed (<code>)` を出し `close_error` に残す (DB と画面をずらさない)。
  - **成功**: 適用値を更新、デスクトップ全体を invalidate、時計は次の tick で描き直し (幅が変わるので §1 の和集合規則)。
  - メッセージは `cfg_close` の**後**に開く (DB を開いたままモーダルに入らない)。
- **Cancel / ESC**: 何も書かない。
- 書き戻しは **gshell 自身の top-level (owner 1、`standalone_loop`)** で行う。handler (S8) や X4、`wm_cycle(Ctx::Wait)` の文脈からは呼ばない。アプリのモーダル (`open` の X3 経路) とは別の purpose なので `fill_resp` / `consume_completed` には流さない (WM 用 purpose の `finish_wm` と同じ扱い)。

## 4. リンクと FFI

- gshell は `libos32cfg.a` を**直接リンク** (shlib 経由ではない。`userland/gshell.elf` の規則に `$(LIBCFG_OBJ)` を足す、PM)。`cfg_backend.c` の `extern KernelAPI *kapi` は gshell の crt0 (`crt0_c.c`) が定義するので追加の初期化は不要 (S2-W の shlib と違い、gshell は通常の OS32X プログラム)。
- `cfg_*` の Rust 側宣言は **`sdk/rust/os32api/src/cfg.rs` に 1 か所** (`extern "C" { fn cfg_open(...); ... }` と `CFG_*` 定数、`#[link]` 属性は付けない)。libos32gui の `cfgro.rs` が持つ**宣言**をこちらへ寄せる (重複した ABI 宣言を 2 か所に置かない)。shlib 用の `kapi` の実体と wrapper 4 本は `cfgro.rs` に残す (アプリ向けの公開窓口は既存の 4 本のまま)。属性なしの未使用 `extern "C"` 宣言は呼び出し・アドレス参照が無ければ未解決シンボルにならない (理由は no_std ではなくそれ)。**ホストハーネスへの接続** (往復 1 non-blocker): `userland/gshell/host/integration.py` は cargo ではなく rustc で組み、`tools/tests/os32api_host.py` は SDK の新しいトップレベルモジュールを自動では取り込まないので、`os32api::cfg` を両ハーネス (gshell host、libos32gui host_tests) から読める形にする作業を S4-W に含める。
- `CfgDb` は不透明ポインタ (`*mut c_void`)。gshell 側は `static mut` に持たず、`load()` / OK の中で open〜close を閉じる。

## 5. ホスト TDD (`userland/gshell/host/`)

既存の `integration.py` (wm_tests.rs / wm_composite_tests.rs をホストで rustc で組んで実行) に `settings_tests.rs` を足す。`cfg_*` は `mocks.rs` の贋物 (呼び順を記録、戻り値を注入):
(1) `load()` が OK / MISSING / CORRUPT / VERSION / ERROR / open 負 / close 負 で既定値と `status` / `close_error` を正しく埋める、(2) `desktop/color` の 0〜15 以外と NOTFOUND が 12 に落ちる、`clock_24h` の 2 が 1 に落ちる、(3) 時計の整形: 24h `00:00` / `23:59`、12h `12:00 AM` / `12:00 PM` / `1:05 PM` / `11:59 PM`、幅 = 文字数 × 8 + 8、(4) ダイアログの状態機械: 開いたときに load が呼ばれる、↑↓←→ / SPACE / RETURN / ESC、値が同じなら OK で `cfg_open` が**呼ばれない**、違えば open(1) → begin → set (変わったキーだけ、順序) → commit → close が**この順で 1 回ずつ**、(5) begin / set / commit の失敗ごとに: メッセージが出て `GuiState.cfg` が**変わらない**、close の呼び順 (close 失敗は (11) のとおり別扱い)、(6) MISSING / VERSION / CORRUPT のときの OK が open(1) を呼ばず状態行の文言だけ、(7) Start メニューの項目数 6 と各項目の順、(8) `desktop::fill` が `cfg.desktop_color` を使う (composite の色の検査)、2 色モードでは使わない、(9) **アプリ 1 本が `Ctx::Wait` の状態で Start → Settings** が `Req::Open` を予約し `should_park` が真になり、top-level の周回で `load()` が呼ばれてモーダルが開く (B1)、(10) OK → `Req::Save` の予約 → top-level で open(1)…close の順 (`finish_wm` の中では `cfg_*` が**呼ばれない**)、(11) commit 成功 + close 失敗で適用値が**更新され**メッセージが `saved, but close failed` (B2)、begin / set / commit 失敗では適用値不変、(12) 起動時通知が `status != CFG_OK || close_error != 0` のとき 1 回だけ出て、OK かつ close 成功では出ない、`close_error` が再 load 成功後も残る、(13) 時計 8 → 7 文字で dirty が旧 ∪ 新 (B4)、(14) X4 / handler の文脈で `cfg_*` の呼び出し回数 0、(15) マウス操作 (row クリック、再クリック、ボタン)、(16) **モーダル枠の競合** (R1): アプリのモーダルが開いている周回では `Req::Open` が消費されず予約が残り `should_park` が真のまま、閉じた次の周回で `load()` → 開く。`Req::Save` はアプリのモーダルが開いていても DB 操作と適用は行われ、通知は `notice` に保持されて `open_wm` が偽の間は出ず、枠が空いた周回で 1 回出る。`open_wm` の戻り値が見られている、(17) リース中 (mono) の Settings の描画が `TEXT` / `WINDOW` だけを使う (R2)、(18) `load()` が get 途中の失敗で `cfg_status` が ERROR に変わった値を採る (open 直後ではなく get 後の status / sqlite コード)。RED → GREEN を `tools/tests/s4_tdd.md` に。

## 6. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| G1 | CUI で `cfg set gshell desktop/color int 3`、`cfg set gshell taskbar/clock_24h int 0` → `os32gui` | デスクトップが色 3、時計が `h:MM AM/PM` (スクリーンショット)。端末を開いて con_sink の `gshell: cfg OK color=3 clock24=0 load=<n>t` を読む。戻して 12 / 1 → 従来の見た目 |
| G2 | GUI の Start → Settings... → color を → で 5 に、clock を 24h に → OK | 即座にデスクトップが 5、時計が `HH:MM`。CUI に戻って `cfg get gshell desktop/color` = 5、`taskbar/clock_24h` = 1 |
| G3 | Settings... → Cancel / ESC、値を変えずに OK | `cfg get` が不変。OK でも `cfg_open` が呼ばれない (端末で `gshell: cfg write skipped` を読む)。**アプリ 1 本 (`gui_demo`) を開いたまま** Settings... → 色を変えて OK でも保存される (B1 の `Ctx::Wait` からの到達)。**`v12_api_test` (モーダルを開くアプリ) のモーダルが開いている間に Start → Settings...** → アプリのモーダルを閉じた後に Settings が開く (R1) |
| G4 | `rm /etc/settings.db` → `os32gui` | 既定値で起動し**起動時通知** `Settings: MISSING - defaults in use. Run 'cfg init' in CUI.` が出る (OK で閉じる)。Settings... の状態行も MISSING、**値を変えてから** OK → `cannot save: MISSING` (無編集の OK は何も出さずに閉じる) |
| G5 | 版 2 の fixture (`tools/mk_settings_db.py --schema-version 2` で生成、`userland/deploy.yaml` に **保護対象でない名前** `/etc/settings.v2.fixture` として配備、tags test) をゲストで `rm /etc/settings.db` → `cp /etc/settings.v2.fixture /etc/settings.db` → `cfg status` が `VERSION 2` であることを先に確認 → `os32gui` (往復 1 の B6: `hsync` / `nhd_deploy.py copy` は settings.db を保護してスキップする) | 起動時通知 `Settings: VERSION 2 (read only)`、Settings... の状態行も同じ、**値を変えてから** OK → `cannot save: VERSION`。終わったら `rm` → `cfg init` で戻す |
| G6 | `tools/gui_gate.py` の `ROW_CUI` = 4 / `ROW_HALT` = 5 で `leave_gshell` と halt 経路が正しい項目に当たる (往復 1 の B5: 定数 6 だけでは行 3 が Settings に当たる) | v11 / v12 の台本が従来どおり通る (確認ダイアログの文言で当たった項目を判定) |
| G7 | S5 の材料: 状態行の `load <n>t save <n>t` と端末の `load=<n>t` | 記録 |

## 7. レビューで見てほしい点

1. 読み書きの位置が決裁 (OS 経由、同時 1 本、yield なし、X4 / handler 禁止) を守るか — 特にモーダルの OK 処理が gshell の top-level イベント処理で完結するか、`open_wm_message` を出す前に `cfg_close` が終わっているか。
2. `load()` の fallback が起動を止めないか、close 失敗の扱い。
3. 時計 12h 表記でタスクバーの矩形計算 (`clock_rect` / 窓ボタン領域 / `refresh_if_hit`) が破綻しないか、`gui_gate.py` の座標。
4. Start メニューの項目追加で既存の台本・受入記録 (行番号) が壊れないか (`start_row` は項目数から導く)。
5. `os32api::cfg` に ABI 宣言を寄せることで libos32gui / gshell / SDK stub の 3 者の整合 (シグネチャは S2 の `libos32cfg.h` が正典)。
6. ホスト TDD の贋物が「呼ばれない」ことまで固定しているか。

## 8. ユーザー判断が要る点

- なし (S0_PLAN §3-4 の決裁の範囲内。壁紙は S4 に含めない = 「壁紙の有無」も出さない、を PM 判断で決めた。異論があれば v1.4 の票へ)。

## 9. レビュー記録

| 版 | 判定 | 要旨 |
|---|---|---|
| 第 2 版 | Request changes | 2 件: R1 予約から消費までにアプリのモーダルが開くと Settings / 通知が消える (`open_wm` は枠が塞がっていると拒否) → 開けなかった要求 / 通知を保持して枠が空いた周回で再試行、`open_wm` の戻り値を見る、R2 16 色リース中の Settings がシステム色を使う → `draw_list` と同じ 2 色分岐、見本は数値のみ。non-blocker: §5 の矛盾 ((5)/(11)、(12))、G4/G5 は編集してから OK、Save の予約に変更マスクと再読込値、get 後の status |
| 第 1 版 | Request changes | 7 件: B1 WM purpose だけでは top-level に移らない (アプリ `OP_WAIT` 中の Settings が handler 文脈で DB に触る) → 予約 + should_park + standalone_loop で消費、B2 commit 成功 + close 失敗を未反映にすると DB と画面がずれる → 適用して別通知、B3 起動時通知が無く kprintf は CUI に残らない → WM 通知 + 端末 / 状態行で採取、B4 可変幅時計の縮小で旧矩形が残る → 和集合、B5 gui_gate の行番号は定数だけでは追従しない → 項目名定数、B6 hsync は settings.db を保護するので VERSION fixture を置けない → 別名で配備して guest cp、B7 gshell の要求 KAPI 49 のまま → 50。non-blocker: close 診断の保持と文言、FFI 集約の根拠、ホストハーネスの接続、UI の値の 3 区別 |

