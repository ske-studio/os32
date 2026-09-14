# TASK_N2 — Host Agent の PRINT / CLIP サービス (ホスト側 Python)

発行: PM (2026-09-14) / 状態: **設計 第 1 版 (Fable 設計レビュー待ち)**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §2 (サービス表) / §4 (印刷) / §6 (運用) / §9 (決裁)、ワイヤは [TASK_N0.md](TASK_N0.md) 第 5 版 §1b (v2、宣言長 + WDATA)。**OS32 側 (KAPI・カーネル) は変えない** — N1 の `host_open`/`host_write`/`host_read`/`host_status`/`host_close` (v51) と宣言長 WDATA でそのまま話す。利用する OS32 コマンド (`lpr`/`hclip`) は N3。

## 0. 範囲
`tools/host_agent.py` に**要求サービスを 6 本足す**だけ (ワイヤ・状態機械・rid 台帳・HELLO は N1 のまま不変)。CLIP は含める、**PUT は v1.4 へ先送り** (§9-5)。印刷は **to-file 既定、pywin32 は任意依存** (§9-1)。置き場は WSL2 のみ (§9-2、実機 Windows は N5)。

| 要求 | 引数 | 本文 | 応答本文 | 実装 |
|---|---|---|---|---|
| `PRINT OPEN <name> <kind>` | kind=`text` (v1) / `raw` (v2 で NOSUP) | 無 | `job <id>` | ジョブ表に 1 本足しスプール `<spool>/<job>.txt` を作る。id はホストが単調採番 (再起動でリセット可) |
| `PRINT DATA <id> <len>` | len ≤ 64KB / 要求 | WDATA (宣言長) | 200 / 409 | 宣言長ぶんを `_finish_body` で受け、ジョブが OPEN 状態ならスプールに追記。未知 / CLOSE 済み id は 409 |
| `PRINT CLOSE <id>` | — | 無 | `pages <n>` / 5xx | スプールを確定し**出力**: 既定は `--to-file` (スプールを保持、または `--print-dir` へ移動)、`--printer` かつ pywin32 があれば Windows 既定プリンタへ。ページ数は改ページ `\f` + 行数/ページ from 概算。失敗は 5xx + STATUS に error |
| `PRINT STATUS <id>` | — | 無 | `queued`/`printing`/`done`/`error <msg>` | ジョブの状態 (非同期の完了確認) |
| `CLIP GET` | — | 無 | UTF-8 テキスト | Windows クリップボードのテキストを読む (`win32clipboard`、無ければ `--clip-file` の内容、どちらも無ければ空 + 200) |
| `CLIP PUT <len>` | len ≤ 4KB (v1) | WDATA (宣言長) | 200 | 受けたテキストを Windows クリップボードへ (`win32clipboard`、無ければ `--clip-file` に書く) |

`GET`/`TIME`/`PING`/`ECHO` は N1 のまま。要求名は大文字・空白区切り・本文が要る要求は末尾に宣言長 (N0 §1b の WDATA)。

## 1. Agent の実装 (`tools/host_agent.py`)
- **ジョブ表** `self.jobs = {id: {"kind","state","spool","pages","error"}}`、`self.next_job` 単調。`state` = `open`/`closing`/`done`/`error`。OPEN で作り、DATA で追記 (open 以外は 409)、CLOSE で確定。
- **要求の振り分け**: `_service_now` に OPEN/CLOSE/STATUS/CLIP GET を足す (小さい応答、即答)。`PRINT DATA` と `CLIP PUT` は**宣言長 + WDATA** なので `_service_now` では応答せず、既存の本文収集 (`_finish_body`) に載せて、受け切ってから追記 / クリップボード書き込み → 200 (N1 の ECHO と同じ経路)。要求行のパースは既存 `parts` を使う。
- **スプール**: `<agent_dir>/spool/<job>.txt` に追記 (§4)。CLOSE で出力。`--to-file` (既定) はスプールを残す (または `--print-dir` へ移す)、`--printer` で pywin32。改ページ `\f`。
- **クリップボード**: `win32clipboard` は**任意 import** (`try: import win32clipboard`)。無い WSL2 環境では `--clip-file <path>` を介す (GET は読む、PUT は書く) ので、pywin32 無しでもテスト・運用できる。
- **境界**: PRINT DATA の宣言長 > 残り (v1 は 1 ジョブの上限を設けない、1 要求 ≤ 64KB を繰り返す)、CLIP PUT > 4KB は要求行の時点で 400。`_finish_body` で verb に応じて分岐 (現状の ECHO / 既定 200 を PRINT DATA / CLIP PUT に拡張)。
- **引数**: `--spool-dir` (既定 `<state>/spool`)、`--print-dir` (to-file の出力先、既定 = spool のまま)、`--printer` (フラグ、pywin32 で既定プリンタへ)、`--clip-file` (pywin32 無しのクリップボード代替)。既存の `--root`/`--file-root` は流用。

## 2. TDD (`tools/tests/test_host_agent.py` を拡張、Python 単体)
既存の贋 OS32 (フレームを直接組む) で:
- PRINT OPEN → `job 1`、DATA (宣言長 = WDATA 長) を 2 回追記 → スプールに連結、CLOSE → `pages n` + スプール内容一致。
- **宣言長と WDATA 長の不一致** (宣言 > 実 WDATA、宣言 < 実) の扱い (N1 の WDATA 契約に従う = Agent は宣言長ぶん受けてから応答)。
- 未知 id / CLOSE 済み id への DATA = 409、STATUS の各状態遷移 (open→closing→done、error)。
- CLIP PUT (宣言長 + WDATA) → `--clip-file` に書かれる、CLIP GET → `--clip-file` の内容を返す (往復)。4KB 超の CLIP PUT = 400。
- `--to-file` の CLOSE がスプールを残す / `--print-dir` へ移す。pywin32 が無い環境で全部通る (win32 系は import 失敗を許容)。
- 既存 25 ケース (HELLO / 台帳 / 墓標 / 枯渇) に回帰なし。

## 3. 受入 (PM)
`python3 -B tools/tests/test_host_agent.py` 全通過。ゲスト受入は N3 (`lpr`/`hclip` 実装後) で実サービスとして: 端末から `lpr /etc/profile` がホストの spool/print-dir に落ちる、`hclip` の往復。N2 単体はホスト Python 試験まで。

## 4. レビューで見てほしい点 (設計)
1. ジョブ表と `_finish_body` の分岐が N1 の WDATA 契約 (宣言長ぶん受けて 200) を壊さないか。
2. pywin32 を任意依存にし `--clip-file` / `--to-file` で pywin32 無しでも完結する設計が妥当か。
3. 409 / 400 / 5xx の返し方が業務結果 (RESPONSE flags 0) で、N1 の制御結果 (flags bit0) と混ざらないか。
4. スプールの並行 (複数ジョブ同時 open) と再起動時のジョブ id リセットの割り切り。
