# T7 — 既存 CUI コマンドを端末で実際に流す

状態: **発行 (2026-09-12、PM)**。決裁 B の順序 (K5 → K6 console → 端末アプリ → K7 入力統合 → **CUI コマンドを端末で流す**) の 5 段目。
前提: K6C (con_sink、端末アプリ `t5a_display`)、K7 (kbd 待ちの park、`kbd_inject`)。すべて feat/gui `474f9ce` に着地・受入済み。

## 0. 目的

端末窓のプロンプトにコマンド名を打って Enter すると、その CUI プログラムが gshell 配下で起動し、
出力が端末窓に出て、打鍵がそのプログラムに届き、終了するとプロンプトに戻る。
v1.3 の「GUI 上の CUI 実行」の縦切りをここで完成させる (shell script / full-screen GFX 復帰は次段)。

## 1. 接点 (確認済み)

- 起動: libos32gui `session_launch(path)` (`userland/rust/libos32gui/src/session.rs:90`、op 66 `GUI_SESSION_LAUNCH`)。
  gshell 側 (`userland/gshell/src/session.rs:146`) は **1〜255B の絶対パス**だけ受け、K5b-W で「LAUNCH は 1 本増やすだけ
  (要求元は畳まれない)」。値はそのまま `exec_start(cmdline)` に渡る (`exec_start` は `const char *cmdline`、引数付き可)。
  既存 pending があれば `OS32_ERR_FULL`。
- 出力: 子の `kprintf` / `shell_print` / `console_write` / `tvram_clear` / `console_set_cursor` は K6C のレコードで端末に届く。
- 入力: 子が `kbd_getchar` で `WAIT_KEY` に park → 端末の `kbd_inject` (K7-A) → gshell が起こす (K7-W)。
- 子の終了: いまは端末に**通知が無い** (K7 受入では `q` → `bye` の表示で見ただけ)。
- 起動失敗: gshell が「Launch failed …」のモーダルを出す (K5b)。exec の `Error: …` 行は con_sink 経由で端末にも出る。

## 2. 設計 (PM 案)

| # | 決定 | 担当 |
|---|---|---|
| E1 | **子の終了レコード**: con_sink に `EXIT` (type 4、payload `id u8`、ヘッダ 2 バイト) を足す。シンク有効中に非シェル ID が回収されるとき (`exec_reclaim_owned` の経路、正常終了 / kill / fault のすべて) にカーネルが積む。`os32_kapi_shared.h` の `CON_SINK_REC_EXIT` / `CON_SINK_HDR_EXIT` を追記 ([C4] Shared Layer、既存 type の値は不変)。`CON_SINK_REC_MAX` は変わらない | K |
| E2 | **プロンプト行**: 端末の最下行に `> ` + 編集中の行 (ローカル編集: 印字可能 ASCII / BS / Enter。FEP の TEXT もそのまま行に入れる)。プロンプト表示中は **打鍵を注入しない** | A |
| E3 | **起動**: Enter で行を確定 → 先頭トークンが `/` で始まればそのまま、そうでなければ `/usr/bin/<名>.bin` → `/bin/<名>.bin` の順に**存在確認** (os32api の open / stat 相当) して最初に見つかった絶対パス + 残りの引数を `session_launch` に渡す。見つからなければ端末に `command not found: <名>` を出す (ローカル)。`OS32_ERR_FULL` (別の LAUNCH が pending) は `busy` を出して行を残す | A |
| E4 | **接続モード**: 起動要求を出したら **接続モード**へ: プロンプトを消し、打鍵は K7-A どおり `kbd_inject`。`EXIT` レコードを受けたらプロンプトに戻る (どの ID が自分の子かは持たない — v1.3 は端末 1 本 / 子 1 本。複数 EXIT が来ても最後のでプロンプトに戻るだけ)。**起動失敗** (gshell のモーダル) の場合は `EXIT` が来ないので、接続モード中の **ESC** で強制的にプロンプトへ戻れるようにする (ESC は子には注入しない) | A |
| E5 | 空行 Enter はプロンプトを再表示するだけ。`exit` はプロンプトから端末自身を終了 (ESC と同じ)。それ以外の内蔵コマンドは作らない (シェルの内蔵コマンド `ls` `cat` 等は常駐シェル内蔵なので端末からは**まだ**呼べない — 次段の「shell script」で `sh -c` 相当を検討) | A |
| E6 | gshell は変更しない。カーネルは E1 だけ | — |

## 3. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| T1 | `kbd_echo` | プロンプトに `kbd_echo` + Enter → 子が起動しバナーが端末に出る → `abc` を打つと `got 0x61 'a'` … が出る → `q` で `bye` が出て**プロンプトに戻る** (`EXIT` レコード) |
| T2 | 出力のみのコマンド | `klibc_test` (または `hal_test`) + Enter → 出力が流れ、終了でプロンプトに戻る。`dropped` 0 |
| T3 | 見つからない | `nosuch` + Enter → `command not found: nosuch`、プロンプトのまま |
| T4 | 起動失敗からの復帰 | `/etc/system.cfg` + Enter → gshell の「Launch failed」モーダル、OK → ESC でプロンプトに戻る |
| T5 | 回帰 | regress 6 本、v86 -t、Start → CUI mode の畳み込み (子が生きていても) |

## 4. 禁止 / 範囲外

- gshell の変更。常駐シェルの内蔵コマンドの端末からの実行。shell script。full-screen GFX の子 (次段)。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。ホスト試験は必須。
