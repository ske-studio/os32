# S4-W — gshell の設定レジストリ消費と設定ダイアログのホスト TDD 記録

票: [`docs/tasks/settings/TASK_S4.md`](../../docs/tasks/settings/TASK_S4.md) 第 4 版 §0 の **S4-W**。
契約の正典は [`S0_FOUNDATION.md`](../../docs/tasks/settings/S0_FOUNDATION.md) §2、
[`DESIGN.md`](../../docs/tasks/settings/DESIGN.md) §3〜§5、
API は [`TASK_S2.md`](../../docs/tasks/settings/TASK_S2.md) §1 と `userland/lib/cfg/libos32cfg.h`。

走らせ方:

```bash
python3 userland/gshell/host/integration.py       # = make check-gshell-host (93 本)
cargo test --manifest-path userland/rust/libos32gui/host_tests/Cargo.toml \
      --target x86_64-unknown-linux-gnu --offline # = make check-gui-host (35 本)
```

コーダーの範囲はホスト TDD までなので、**ゲスト受入 (票 §6 の G1〜G7) は未実行** ([V4])。
`make all` / `make check` / 配備 / エミュレータには触っていない。対象を絞った
`cargo build` (gshell / libos32gui) と `tools/check_constraints.py` だけを走らせた。

---

## 0. 組み方

| | |
|---|---|
| 実装 | `userland/gshell/src/settings.rs` (新規)、`lib.rs` / `modal.rs` / `startmenu.rs` / `desktop.rs` / `taskbar.rs` / `multiapp.rs` / `wm.rs`、`sdk/rust/os32api/src/cfg.rs` (新規) |
| 試験 | `userland/gshell/host/settings_tests.rs` (新規、20 関数)、贋物は `host/mocks.rs` |
| 実行 | `userland/gshell/host/integration.py` (rustc で組む。cargo ではない) |

`src/settings.rs` の末尾が `#[cfg(test)] #[path = "../host/settings_tests.rs"] mod tests;` で
試験を取り込み、`integration.py` が gshell の**実モジュール**をホスト ABI の代用
(`host/mocks.rs`) と一緒に 1 本の `--test` バイナリへ組む。既存の `wm_tests.rs` /
`wm_composite_tests.rs` と同じ 1 プロセスに載るので、試験は `--test-threads=1` で直列。

`cfg_*` は `mocks.rs` の贋物 (`CfgFake`)。**実 DB も SQLite も出てこない** — それは
S2-C の `tools/tests/test_cfg.py` の領分で、ここで押さえるのは **gshell の判断**:

- いつ `cfg_open` するか、**いつ呼ばないか** (X4 / handler / 無編集の OK / status ≠ OK)
- 1 周回の中で `open → begin → set → commit → close` を閉じているか
- 失敗の段ごとに適用値が動くか動かないか
- モーダル枠が塞がっているときの予約と通知の保持 (R1)

贋物が記録するのは **DB を動かす操作だけ** (`Open` / `GetInt` / `Begin` / `SetInt` /
`Commit` / `Rollback` / `Close`)。純粋な照会 (`cfg_status` / `cfg_last_sqlite` /
`cfg_schema_version` / `cfg_last_close_error`) は数えない — 見たいのは順序だから。

### ホストハーネスへの接続 (票 §4 の non-blocker)

1. `tools/tests/os32api_host.py` は SDK の新しいトップレベルモジュールを自動では
   取り込まないので、生成する adapter に `#[path=".../cfg.rs"] pub mod cfg;` を 1 行足した
   (`gui` と同じ扱い)。これで gshell の host 試験から `os32api::cfg::*` が見える。
2. `libos32gui/host_tests` は os32api に依存しない独立クレートなので、
   `#[path = "../../../../../sdk/rust/os32api/src/cfg.rs"] pub mod cfgabi;` で**実ファイルを
   直に**取り込む。`cfgro.rs` が見る名前は本体でもホストでも `crate::cfgabi` の 1 つ
   (本体側は `lib.rs` の `pub use os32api::cfg as cfgabi;`)。`cfgro.rs` は
   **os32api を名指ししない**という S2 の性質をそのまま保った。

---

## 1. RED → GREEN

### RED 1 — 試験だけ足した最初の実行 (コンパイルが通らない)

```text
error[E0425]: cannot find function `root_label` in module `startmenu`
error[E0425]: cannot find function `set_sys_time` in module `mocks`
error[E0425]: cannot find function `settings_hit_rects` in module `modal`
error[E0599]: no method named `msg_bytes` found for ... `&mut Modal`
error[E0603]: function `adopt_running` is private
```

観測点が足りなかった。足したもの (どれも**実装の分岐を増やさない**窓口):

| 足したもの | なぜ要るか |
|---|---|
| `startmenu::root_label(i)` | 「6 項目・この順」を固定する (§5 の (7))。`item_label` の root 枝もこの表から引くようにして二重管理をやめた |
| `modal::msg_bytes()` / `settings_hit_rects()` / `settings_values()` | 通知の文言、行 / ボタンの当たり矩形、編集値 |
| `taskbar::clock_rect_for(st, n)` / `clock_len()` / `format_clock()` | 幅の式と整形を直接叩く (§5 の (3)(13)) |
| `mocks::set_sys_time()` | 時計の整形に時刻を与える (従来は `sys_time` が常に 0 の固定スタブ) |
| `multiapp::adopt_running` を `pub` に | 「アプリ 1 本が走っている」状態を実物の表で作る |

### RED 2 — リンクが通ってから (15 本)

```text
test result: FAILED. 77 passed; 15 failed
    settings::tests::s04 s05 s06 s07b s09 s11 s12 s13 s14 s15 s16 s16b s17
    wm::wm_composite_tests::regression_w3_lower_chrome_never_enters_a_higher_windows_client
    wm::wm_composite_tests::regression_w3_lower_title_text_is_dropped_instead_of_spilling
```

最後の 2 本は**既存の試験**で、原因は新しい試験ではなく**ホストの初期化漏れ**だった:
モーダル / Start メニュー / 時計は**プロセスに 1 つの `static`** なので、前の試験が
開けたまま終わると次の試験の `composite_rect` に写り込む (`(216,145)` が
`SENTINEL` 200 ではなく `GUI_COLOR_TEXT` 0 になった = 設定ダイアログが前面窓の
クライアント面に重なって描かれた)。S4 以前は「モーダルを開けたまま終わる試験」が
無かったので表に出ていなかった。

直し: `mocks::init()` に `modal::reset()` / `startmenu::reset()` / `taskbar::reset()` /
`settings::reset()` を足した (`fullscreen::reset()` / `multiapp::reset()` と同じ扱い)。
**実装の振る舞いは変えていない** — 試験の初期化だけ。

### RED 3 — 残り 1 本 (試験側の誤り)

```text
assertion `left == right` failed: 0 の左で 15 に回らない
```

色 3 → `→` で 4 → `←` 4 回で **0** (まだ折り返していない)。試験の数え間違いだったので
「4 回で 0、もう 1 回で 15」と書き直した。実装 (`(v + delta + 16) % 16`) は正しい。

### GREEN

```text
userland/gshell/host/integration.py : 93 passed; 0 failed
libos32gui host_tests               : 34 passed + 1 passed (init_gate)
cargo build (gshell, i686-os32-none): OK (警告 0)
cargo build (libos32gui)            : OK
tools/check_constraints.py          : 制約チェック OK — 規則 16 件
```

---

## 2. 固定した分岐 (20 本 / 票 §5 の (1)〜(18))

| # | 試験 | 固定した振る舞い |
|---|---|---|
| S01 | `load()` の 7 経路 | OK = 読めた値 / MISSING・CORRUPT・VERSION・ERROR = 既定値 + status (VERSION は版数をそのまま持つ、ERROR は sqlite コード) / `cfg_open` が負 = `status = -1` で **close を呼ばない** / `0` を返しながら NULL も同じ / `cfg_close` が負でも**読めた値は使い** `close_error` に残す。全経路で `Open(0) → GetInt ×2 → Close` を閉じきる |
| S02 | 範囲外と NOTFOUND | `desktop/color` は 0〜15 以外 (負・16・`i32::MIN/MAX`) が 12 へ、`taskbar/clock_24h` は 0/1 以外 (2 など) が 1 へ。行が無い (NOTFOUND) も既定 |
| S03 | 時計の整形と幅 | 24h `00:00` / `23:59`、12h `12:00 AM` / `12:00 PM` / `1:05 PM` / `11:59 PM` / `12:01 AM` / `9:07 AM` (**時は空白詰めしない**)。幅 = 文字数 × 8 + 8 (5→48 / 7→64 / 8→72)、右端は文字数によらず揃う |
| S04 | ダイアログの状態機械 | 開いた周回で `load()` が走る / `→` `←` で色が 0..15 を循環 / `↓` `↑` で行移動 (0〜1 で止まる) / SPACE で clock がトグル / ESC は何も書かない / **無編集の OK は `cfg_open` を呼ばず** `gshell: cfg write skipped` を出して**メッセージも出さない** / 編集後の OK は `finish_wm` の中では 1 本も `cfg_*` を呼ばず、top-level で `Open(1) → Begin → SetInt(変わった key だけ) → Commit → Close` |
| S05 | begin / set / commit の失敗 | 段ごとに **適用値が動かない**、`Close` は必ず通る (rollback は close の仕事)、文言は `save failed: <段> (<code>)` |
| S06 | MISSING / CORRUPT / VERSION | **編集後の** OK が `cfg_open(1)` を呼ばず `cannot save: <status>`。**無編集の OK は何も出さずに閉じる** |
| S07 | Start メニュー | root は 6 項目で `Programs` / `File Manager` / `Run...` / `Settings...` / `CUI mode` / `Shut Down`。`Settings...` は index 3、`CUI mode` は 4、`Shut Down` は 5 (= `gui_gate.py` の `ROW_*`) |
| S07b | Settings... の選択 | 予約が立つだけ。**メニューの文脈で `cfg_*` を 1 本も呼ばず、ダイアログも開かない** |
| S08 | `desktop::fill` | `st.cfg.desktop_color` を塗る (色 3 を入れれば 3)。**2 色モード (リース中) は従来どおり `TEXT` / `WINDOW` の市松だけ** |
| S09 | アプリが `Ctx::Wait` (B1) | 予約の時点では DB に触らない → `should_park` が真 → top-level の周回で初めて `load()` が走りダイアログが開く → 消費後は `should_park` が偽に戻る |
| S09b | 通知だけの pending (往復 3 non-blocker 2) | `req = None, notice = Some` でも **実スケジューラ経路** (`should_park` → `note_parked` → `pick`) が top-level へ返す。`pick` は `pick_poll` の門で **0** (誰も起こさない)。そこで通知が出て、出し終われば門が閉じる |
| S10 | OK は予約だけ | (S04 に統合) `finish_wm` の中の `cfg_*` 呼び出し回数 = 0 |
| S11 | commit 成功 + close 失敗 (B2) | **適用値を更新して**から `saved, but close failed (-5)`、`close_error` にも残る |
| S12 | 起動時通知 | OK かつ close 成功なら**出ない** (kprintf の 1 行 `gshell: cfg OK color=12 clock24=1 load=<n>t` は出る) / MISSING・VERSION・ERROR・close 失敗は**1 回だけ**出て、閉じたら二度と出ない / `close_error` は**再 load が成功しても消えない** |
| S13 | 時計 8 → 7 文字 (B4) | 縮む更新で**旧矩形の左端 8px が dirty に入る** (新矩形だけなら残る) |
| S14 | X4 / X3 の文脈 | 予約を立てたまま `wm_cycle(Ctx::Pump)` と `wm_cycle(Ctx::Wait)` を回しても `cfg_*` の呼び出しは **0 本**。top-level の消費で初めて 1 回開く |
| S15 | マウス | 別の行のクリック = 選択だけ / 同じ行の再クリック = 値が進む / Cancel は書かない / OK は予約が立ち適用まで行く |
| S16 | モーダル枠の競合 (R1) | アプリの `MODAL_OPEN` は**拒否しない**。枠が塞がっている周回では `Req::Open` を消費せず `load()` もせず、予約が残り `should_park` は真のまま。アプリのモーダルが閉じた次の周回で開き、`load()` はその 1 回だけ |
| S16b | 保存と通知の分離 (R1) | `Req::Save` は枠が塞がっていても**その周回で** DB 操作と適用まで済ませる。通知だけが `notice` に残り (`open_wm` の戻りを見ている)、アプリのモーダルは潰さない。枠が空いた周回で **1 回だけ**出る |
| S17 | リース中の描画 (R2) | 設定ダイアログが `TEXT` (0) / `WINDOW` (7) しか置かない。**色見本を描かない** (数値と `(preview off: palette leased)` だけ)。16 色に戻せば見本が出る |
| S18 | status は get の後 | open 直後 OK → get の途中で `CFG_ERROR` になった DB で、`load()` は **ERROR** と sqlite コードを採り、値は既定へ落とす |

libos32gui 側 (`make check-gui-host`) は S2 の 35 本が**宣言の移動後もそのまま通る**ことを
確認しただけで、ケースは 1 本も足していない (`s2_tdd.md` §W が正典)。

---

## 3. 票からずらした点

| 点 | 票 | 実装 | 理由 |
|---|---|---|---|
| purpose の名前 | `PURPOSE_SETTINGS` / `PURPOSE_CFG_NOTICE` | `WM_PURPOSE_SETTINGS` / `WM_PURPOSE_CFG_NOTICE` | `modal.rs` の既存 5 本が `WM_PURPOSE_*` (WM owned の印)。同じ表に別の綴りを混ぜない |
| `cfg_open` が負のときの起動時通知 | 文言 5 種のみ | `Settings: ERROR open=<rc> - defaults in use` を 6 種目として足した | 票は「open 負」も通知の条件に挙げているが対応する文言が無い。`rc` は SQLite コードではないので `sqlite=` とは書けない |
| status が OK 以外**かつ** close も失敗 | 1 通知 | status の文言に `; close failed (<code>)` を継ぐ | 通知は 1 本 (後から来た通知が先のものを上書きしない) なので、捨てずに継いだ |
| `cannot save: <status>` の出し方 | 「保存せずメッセージ」 | `notice` 経路に載せた (top-level の次の周回で出す) | 枠の競合 (R1) の扱いを 1 本化するため。`open_wm` の戻りを見る場所を 1 か所に保てる |
| 保存の前の status 再確認 | 記載なし | `Req::Save` の消費で `cfg_open(1)` の直後に `cfg_status` を見て、OK でなければ 1 バイトも書かずに閉じる | 予約から消費までの間に CUI で `cfg init` / `rm` が走りうる。`cfg_begin` が `INVAL` を返すので実害は無いが、**書きに行った形跡を残さない**ほうが診断が素直 |

---

## 4. ホストで踏めなかったもの ([V4])

- **ゲスト受入 G1〜G7 は 1 つも実行していない**。実 SQLite・実 `/etc/settings.db`・
  実画面 (色 / 時計 / スクリーンショット)・`cfg` コマンドとの往復・端末アプリで
  `con_sink` の `gshell: cfg ...` を読む経路は、どれもホストの贋物では踏めない。
- `kprintf` の**書式解釈**は踏んでいない。贋物は可変長引数を扱えないので
  固定 3 引数 (`attr`, `"%s"`, 1 本) で受けている。gshell 側が渡すのもその形だけ。
- `libos32cfg.a` の**リンク**は踏んでいない (`build/programs.mk` の
  `$(LIBCFG_OBJ)` は PM の担当。`cargo build` は Rust 側だけを組み、
  `cfg_*` は未解決のまま `libgshell.a` に残る)。
- 実機のパレットリース中の見え方 (R2) は**画素の色番号**までしか見ていない。
