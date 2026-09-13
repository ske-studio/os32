# S4 — gshell が設定レジストリを読む最初の消費者 + 設定ダイアログ

状態: **設計 第 1 版 (Codex 設計レビュー 往復 1/3 待ち)**。前提: S2 完了 (`libos32cfg` = `userland/lib/cfg/libos32cfg.h`、KAPI v50、`cfg` コマンド、libos32gui の `os32gui_cfg_*` 4 本、main `e09c458`)。決裁: [S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §3-4 (最初の消費者は数キー)、§4 (設定の読み書きは OS 経由だけ、接続は同時 1 本、open〜close の間に yield しない)。
契約の正典: [S0_FOUNDATION.md](S0_FOUNDATION.md) §2、[DESIGN.md](DESIGN.md) §3〜§5、[TASK_S2.md](TASK_S2.md) §1 (API と規則 1〜8)。gshell の契約は docs/tasks/gui/ (S6 = CUI 復帰、S8 = handler の禁止事項、X4 = 描画 callback)。
規約: gshell は Rust (no_std、`os32api` のみ依存)、[C1] は C 側に、[V2] deploy.yaml、コーダーは worktree + ホスト TDD のみ。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S4-W** | gshell の起動時読み込み (`gshell` scope の 2 キー)、Start メニュー「Settings...」、WM 内蔵の設定ダイアログ (読み → 変更 → OK で書き戻し → 即時反映)、ホスト TDD | W | `userland/gshell/src/{settings.rs (新規), lib.rs, startmenu.rs, desktop.rs, taskbar.rs, modal.rs, ffi.rs}`、`sdk/rust/os32api/src/cfg.rs` (新規: `cfg_*` の extern "C" 宣言を 1 か所に。libos32gui の `cfgro.rs` もこれを使うよう寄せる)、`userland/gshell/host/*` |
| 共有 (PM) | `build/programs.mk` (gshell.elf のリンクに `$(LIBCFG_OBJ)`)、`assets/settings/defaults.tsv` (`desktop/color` の既定を 12 に = 現行 `GUI_COLOR_DESKTOP`)、`tools/gui_gate.py` (`START_MENU_ITEMS` 5 → 6)、`tools/emu_agent/agent.py`、`docs/tasks/gui/` の Start メニュー記述 | PM | — |

S4 に**含めない**: 壁紙の描画 (`desktop/wallpaper` は S4 では読まない・出さない。画像の読み込み経路 (MGX) と 386 の再描画コストの実測を S5 で見てから v1.4 で)、窓位置の記憶、`assets/filetypes` の DB 移行 (次段)、他アプリへの変更通知 (v1.x は「自分で読み直す」、DESIGN §7)。

## 1. 読むキーと意味 (`gshell` scope)

| key | 型 | 意味 | 範囲外 / 欠損 | 既定 (defaults.tsv) |
|---|---|---|---|---|
| `desktop/color` | int | デスクトップ背景のシステム色 index (0〜15、`GUI_COLOR_*` と同じ 16 色) | 0〜15 以外・NOTFOUND → `GUI_COLOR_DESKTOP` (12) | **12** (S0-T の `1` は誤りなので改める。既定で見た目が変わらない) |
| `taskbar/clock_24h` | int | 1 = `HH:MM` (現行)、0 = `h:MM AM` / `h:MM PM` (12 時間、時は空白詰めなし) | 0 / 1 以外・NOTFOUND → 1 | 1 |

- 読むのは **起動時 1 回** (`main` → gfx attach の後、最初の合成の前) と、**設定ダイアログを開いたとき** (現在値の表示のため)。X4 / IRQ / 描画 callback / handler (S8) では DB に触らない。
- `desktop/color` の反映: `desktop::fill` が `GUI_COLOR_DESKTOP` 定数の代わりに `GuiState.cfg.desktop_color` (u8) を使う。2 色モード (lease、市松) は従来どおり色を使わない。
- `clock_24h` の反映: `taskbar::tick_clock` の整形が `GuiState.cfg.clock_24h` を見る。12 時間表記は最長 8 文字 (`12:59 PM`) なので `clock_rect` の幅を **文字数から** 決める (現行は 5 文字固定。窓ボタン領域が 24px 狭くなるだけで、レイアウトの他の座標は不変)。`tools/gui_gate.py` の時計座標 (614, H-12) は 24h のときのまま。

## 2. 起動時の読み込み (`settings.rs`)

```rust
pub struct GuiCfg { pub desktop_color: u8, pub clock_24h: bool, pub status: i32 /* CFG_* or -1 = open 失敗 */, pub sqlite: i32 }
pub fn load() -> GuiCfg      /* cfg_open(&db, 0) → cfg_get_int ×2 → cfg_close。全経路で既定値を埋める */
```
- `cfg_open` が負 (引数 / メモリ) → 既定値、`status = -1`。`cfg_status` が MISSING / CORRUPT / VERSION / ERROR でも **起動は止めない** (既定値で動く。DESIGN §5 の fallback)。`VERSION` は読める値をそのまま使う。
- `cfg_close` の失敗は **握りつぶさない**: `GuiCfg.close_error` に残し、設定ダイアログの状態行に出す (隔離 slot が 1 本消えたことを利用者が知る手段)。起動の可否には影響しない。
- 所要時間は S5 で計測する (`cfg get` 1 回 = 約 50 tick は `/api/cmd` 往復込み。プロセス内の open + 2 get + close は数 tick の見込み)。**ログ**: `kprintf` で `gshell: cfg <status> color=<n> clock24=<0/1> (<tick>)` を 1 行 (CUI 側のコンソールに残る。受入 G1 の材料)。

## 3. 設定ダイアログ (WM 内蔵モーダル、`modal.rs` の新 purpose `PURPOSE_SETTINGS`)

- 入口: Start メニューの root に **「Settings...」を Run... の次 (index 3) に追加** (`IT_SETTINGS`、`ROOT_ITEMS` 5 → 6、CUI mode / Shut Down は 1 行下がる)。`tools/gui_gate.py` の `START_MENU_ITEMS` を 6 に (PM)。既存の台本 (`leave_gshell` は行 3 → **行 4**) は `start_row` から導いているので定数の更新だけで追従する。
- 見た目 (既存モーダルの部品だけで組む。新しいウィジェットは作らない): タイトル `Settings`、本文 2 行 (list band の row 0 / 1 を流用)、下にボタン `OK` / `Cancel`。
  - row 0: `Desktop color : <n>  [<色の 16px 見本>]`、row 1: `Clock : 24h` / `Clock : 12h`。
  - 選択行は SEL_BG で反転。↑ / ↓ で行移動、← / → / SPACE で値を変える (color は 0..15 を循環、clock はトグル)、RETURN = OK、ESC = Cancel。マウスは row クリックで選択 + 同じ row の再クリックで値を進める、ボタンはクリック。**入力欄 (テキスト) は使わない** (数値入力の検証を持ち込まない)。
  - 状態行 (本文 3 行目、小さく): `settings.db: OK` / `MISSING - run 'cfg init' in CUI` / `CORRUPT` / `VERSION <n> (read only)` / `ERROR sqlite=<code>` / `close failed (<code>)`。
- 開いたとき: `settings::load()` を**もう一度**呼んで現在値を出す (起動後に `cfg` コマンドで変わった値を反映する。DESIGN §7「自分で読み直す」)。読めなければ既定値を出し、状態行に理由。
- **OK** (書き戻し点。決裁「設定が変わったときだけ」): 値が起動時 / 開いたときの値と**同じなら書かない** (open すらしない)。違うときだけ **1 つのイベント処理の中で** `cfg_open(&db, 1)` → `cfg_begin` → `cfg_set_int` (変わったキーだけ) → `cfg_commit` → `cfg_close` を完結させる (間に yield / 合成 / 他イベント処理を挟まない)。失敗 (`cfg_status != OK` で writable でない、begin / set / commit の負、close の負) は **メッセージダイアログ** (`open_wm_message`、OK のみ) で理由を出し、値は反映しない (DB と画面がずれない)。成功したら `GuiState.cfg` を更新し、デスクトップ全体を invalidate、時計は次の tick で描き直し。
- **Cancel / ESC**: 何も書かない。
- 書き戻しは **gshell 自身のイベント処理 (top-level、owner 1)** で行う。handler (S8) や X4 からは呼ばない。アプリのモーダル (`open` の X3 経路) とは別の purpose なので `fill_resp` / `consume_completed` には流さない (WM 用 purpose の `finish_wm` と同じ扱い)。

## 4. リンクと FFI

- gshell は `libos32cfg.a` を**直接リンク** (shlib 経由ではない。`userland/gshell.elf` の規則に `$(LIBCFG_OBJ)` を足す、PM)。`cfg_backend.c` の `extern KernelAPI *kapi` は gshell の crt0 (`crt0_c.c`) が定義するので追加の初期化は不要 (S2-W の shlib と違い、gshell は通常の OS32X プログラム)。
- `cfg_*` の Rust 側宣言は **`sdk/rust/os32api/src/cfg.rs` に 1 か所** (`extern "C" { fn cfg_open(...); ... }` と `CFG_*` 定数)。libos32gui の `cfgro.rs` が持つ宣言をこちらへ寄せる (重複した ABI 宣言を 2 か所に置かない)。`os32api` は `#![no_std]` のライブラリで、宣言だけなら実体が無くてもリンク対象に入らない (使わないクレートには影響しない)。
- `CfgDb` は不透明ポインタ (`*mut c_void`)。gshell 側は `static mut` に持たず、`load()` / OK の中で open〜close を閉じる。

## 5. ホスト TDD (`userland/gshell/host/`)

既存の `integration.py` (wm_tests.rs / wm_composite_tests.rs をホストで cargo test) に `settings_tests.rs` を足す。`cfg_*` は `mocks.rs` の贋物 (呼び順を記録、戻り値を注入):
(1) `load()` が OK / MISSING / CORRUPT / VERSION / ERROR / open 負 / close 負 で既定値と `status` / `close_error` を正しく埋める、(2) `desktop/color` の 0〜15 以外と NOTFOUND が 12 に落ちる、`clock_24h` の 2 が 1 に落ちる、(3) 時計の整形: 24h `00:00` / `23:59`、12h `12:00 AM` / `12:00 PM` / `1:05 PM` / `11:59 PM`、幅 = 文字数 × 8 + 8、(4) ダイアログの状態機械: 開いたときに load が呼ばれる、↑↓←→ / SPACE / RETURN / ESC、値が同じなら OK で `cfg_open` が**呼ばれない**、違えば open(1) → begin → set (変わったキーだけ、順序) → commit → close が**この順で 1 回ずつ**、(5) begin / set / commit / close の失敗ごとに: メッセージが出て `GuiState.cfg` が**変わらない**、rollback / close の呼び順、(6) MISSING / VERSION / CORRUPT のときの OK が open(1) を呼ばず状態行の文言だけ、(7) Start メニューの項目数 6 と各項目の順、(8) `desktop::fill` が `cfg.desktop_color` を使う (composite の色の検査)、2 色モードでは使わない。RED → GREEN を `tools/tests/s4_tdd.md` に。

## 6. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| G1 | CUI で `cfg set gshell desktop/color int 3`、`cfg set gshell taskbar/clock_24h int 0` → `os32gui` | 起動ログ `gshell: cfg OK color=3 clock24=0`、デスクトップが色 3、時計が `h:MM AM/PM`。戻して 12 / 1 → 従来の見た目 |
| G2 | GUI の Start → Settings... → color を → で 5 に、clock を 24h に → OK | 即座にデスクトップが 5、時計が `HH:MM`。CUI に戻って `cfg get gshell desktop/color` = 5、`taskbar/clock_24h` = 1 |
| G3 | Settings... → Cancel / ESC、値を変えずに OK | `cfg get` が不変。OK でも `cfg_open` が呼ばれない (kprintf の計器 `gshell: cfg write skipped` で確認) |
| G4 | `rm /etc/settings.db` → `os32gui` → Settings... | 既定値で起動、状態行 `MISSING - run 'cfg init' in CUI`、OK で書かない (メッセージ) |
| G5 | `cfg` で meta を 2 にした DB (S2 のホスト fixture を `hsync` で置く) → `os32gui` | 読める値で起動、状態行 `VERSION 2 (read only)`、OK は拒否 |
| G6 | `tools/gui_gate.py` の既存台本 (`leave_gshell` = CUI mode の行) が 6 項目で正しく当たる | v11 / v12 の台本が従来どおり通る |
| G7 | S5 の材料: 起動ログの tick、Settings OK の tick | 記録 |

## 7. レビューで見てほしい点

1. 読み書きの位置が決裁 (OS 経由、同時 1 本、yield なし、X4 / handler 禁止) を守るか — 特にモーダルの OK 処理が gshell の top-level イベント処理で完結するか、`open_wm_message` を出す前に `cfg_close` が終わっているか。
2. `load()` の fallback が起動を止めないか、close 失敗の扱い。
3. 時計 12h 表記でタスクバーの矩形計算 (`clock_rect` / 窓ボタン領域 / `refresh_if_hit`) が破綻しないか、`gui_gate.py` の座標。
4. Start メニューの項目追加で既存の台本・受入記録 (行番号) が壊れないか (`start_row` は項目数から導く)。
5. `os32api::cfg` に ABI 宣言を寄せることで libos32gui / gshell / SDK stub の 3 者の整合 (シグネチャは S2 の `libos32cfg.h` が正典)。
6. ホスト TDD の贋物が「呼ばれない」ことまで固定しているか。

## 8. ユーザー判断が要る点

- なし (S0_PLAN §3-4 の決裁の範囲内。壁紙は S4 に含めない = 「壁紙の有無」も出さない、を PM 判断で決めた。異論があれば v1.4 の票へ)。
