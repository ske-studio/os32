# TASK_N2 — Host Agent の PRINT / CLIP サービス (ホスト側 Python)

発行: PM (2026-09-14) / 状態: **設計 第 2 版 (往復 1 の blocker 6 件を反映: 完了後も WDATA を必ず ACK、宣言長 0 / 範囲外は要求行で 400、非 200 の本文も配送、スプール名を一意に、CLIP は WSL2 の clip.exe / powershell 実体 + 無ければ 503、先送り PUT は 501。往復 2 待ち)**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §2 (サービス表) / §4 (印刷) / §6 (運用) / §9 (決裁)、ワイヤは [TASK_N0.md](TASK_N0.md) 第 5 版 §1b (v2、宣言長 + WDATA)。**OS32 側 (KAPI・カーネル) は変えない** — N1 の `host_open`/`host_write`/`host_read`/`host_status`/`host_close` (v51) と宣言長 WDATA でそのまま話す。利用する OS32 コマンド (`lpr`/`hclip`) は N3。

## 0. 範囲
`tools/host_agent.py` に**要求サービスを 6 本足す**だけ (ワイヤ・状態機械・rid 台帳・HELLO は N1 のまま不変)。CLIP は含める、**PUT は v1.4 へ先送り** (§9-5)。印刷は **to-file 既定、pywin32 は任意依存** (§9-1)。置き場は WSL2 のみ (§9-2、実機 Windows は N5)。

| 要求 | 引数 | 本文 | 応答本文 | 実装 |
|---|---|---|---|---|
| `PRINT OPEN <name> <kind>` | kind=`text` (v1) / `raw` → 501 | 無 | `job <id>` | ジョブ表に 1 本足しスプール **`<spool>/<epoch>-<job>.txt`** (再起動をまたいで一意、B4) を作る。id は `state_dir/job.txt` に永続する単調カウンタ。未知 kind → 400。`<name>` はパスに使わない (ログ用) |
| `PRINT DATA <id> <len>` | **1 ≤ len ≤ 65536** | WDATA (宣言長) | 200 / 400 / 409 | `len` を `^[0-9]+$` で読み範囲外 (0 / 非数 / 超過) → **要求行の時点で 400** (WDATA 無し、B2)。宣言長ぶんを `_finish_body` で受け、ジョブが open ならスプールに追記 → 200。未知 / close 済み id → 409。**完了後・応答後も WDATA seq は必ず累積 ACK する** (本文は捨てる、B1) |
| `PRINT CLOSE <id>` | — | 無 | `pages <n>` / 500 (本文 `error <msg>`) | スプールを確定し**出力**: 既定 `--to-file` (`--print-dir` があればそこへ一意名で移す、既定はスプールに残す)、`--printer` かつ win32print があれば Windows 既定プリンタへ。ページ数は `\f` + 概算行数。失敗 → **500 + 本文 `error <msg>`** (B3 で本文も配送される)、job.state=error。未知 / 二重 close → 409 |
| `PRINT STATUS <id>` | — | 無 | `queued` / `done` / `error <msg>` | 状態 (単一スレッドなので `printing` は観測されない)。open→`queued`、close 成功→`done`、失敗→`error <msg>` (200 の本文)。未知 id → 409 |
| `CLIP GET` | — | 無 | UTF-8 テキスト / 503 | クリップボードを読む (下の `--clip` バックエンド)。改行は CRLF→LF、64KB 超は切って 200。**バックエンドが無ければ 503** (空クリップの 200 + 空と区別、B5) |
| `CLIP PUT <len>` | **1 ≤ len ≤ 4096** | WDATA (宣言長) | 200 / 400 / 503 | 範囲外 → 要求行で 400 (B2)。テキストをクリップボードへ (LF→CRLF)。**バックエンドが無ければ 503、黙って捨てない** (B5)。完了後の WDATA も ACK (B1) |

`GET`/`TIME`/`PING`/`ECHO` は N1 のまま。要求名は大文字・空白区切り・本文が要る要求は末尾に宣言長 (N0 §1b の WDATA)。

## 1. Agent の実装 (`tools/host_agent.py`)
- **ジョブ表** `self.jobs = {id: {"kind","state","spool","pages","error"}}`、`self.next_job` 単調。`state` = `open`/`closing`/`done`/`error`。OPEN で作り、DATA で追記 (open 以外は 409)、CLOSE で確定。
- **要求の振り分け**: `_service_now` に OPEN/CLOSE/STATUS/CLIP GET を足す (小さい応答、即答)。`PRINT DATA` と `CLIP PUT` は**宣言長 + WDATA** なので `_service_now` では応答せず、既存の本文収集 (`_finish_body`) に載せて、受け切ってから追記 / クリップボード書き込み → 200 (N1 の ECHO と同じ経路)。要求行のパースは既存 `parts` を使う。
- **スプール**: `<agent_dir>/spool/<job>.txt` に追記 (§4)。CLOSE で出力。`--to-file` (既定) はスプールを残す (または `--print-dir` へ移す)、`--printer` で pywin32。改ページ `\f`。
- **クリップボード**: `win32clipboard` は**任意 import** (`try: import win32clipboard`)。無い WSL2 環境では `--clip-file <path>` を介す (GET は読む、PUT は書く) ので、pywin32 無しでもテスト・運用できる。
- **境界**: PRINT DATA の宣言長は 1〜65536、CLIP PUT は 1〜4096。**0 / 非数 (`+5` `-5` `5_0` を含む — Python `int()` ではなく正規表現で読む) / 上限超は要求行の時点で 400** (WDATA を待たない、B2)。1 ジョブに上限は設けず 1 要求 ≤ 64KB を繰り返す。`_finish_body` は verb で分岐 (ECHO / PRINT DATA / CLIP PUT / それ以外 501)。
- **引数**: `--spool-dir` (既定 `<state>/spool`)、`--print-dir` (to-file の出力先、既定 = spool のまま。移す名前は `<epoch>-<job>.txt` で一意)、`--printer` (フラグ、win32print で既定プリンタへ)、`--clip {auto,win32,wsl,file:<path>,none}` (既定 `auto` = win32 が import できれば win32、駄目なら WSL2 の `clip.exe`/`powershell` が居れば wsl、どちらも無ければ none=503)。既存の `--root`/`--file-root` は流用。`HostAgent.__init__` は `spool_dir`/`print_dir`/`clip`/`printer` を受ける (試験は CLI を通さない)。
- **CLIP バックエンド** (B5): `win32` = `win32clipboard` (Windows ネイティブ Python)。`wsl` = PUT は `clip.exe` に UTF-16LE を stdin、GET は `powershell.exe -NoProfile -Command "Get-Clipboard -Raw"` を UTF-16/UTF-8 でデコード。`file:<path>` = そのファイルに読み書き (試験・pywin32 無しの運用)。`none` = GET/PUT とも 503。
- **B1 の ACK 規則**: `_on_wdata` は `resp` 確定後 / `decl` 到達後でも、`seq == last_seq + 1` なら `last_seq` を進めて (本文は捨てて) **必ず累積 ACK を返す**。重複 (`seq ≤ last_seq`) も ACK。これで「宣言 < 実」= 超過分は捨てて ACK・応答は 1 回、「宣言 > 実 (RELEASE で打ち切り)」= スプールは伸びず応答無し、が定まる。
- **B3 の配送規則**: `_answer` は `total > 0` なら status に依らず `deliver` を作る (200 限定をやめる)。409 / 500 / 503 に本文を付けても OS32 が WINDOW→DATA で読み切れる。既存 `GET /status/410` の本文もこれで読める (N1 の潜在バグ)。
- **B6 の既定**: `_finish_body` の既定 (未知 verb の本文) は **501** (200 をやめる)。先送りの `PUT /file/` は `_serve` で宣言長を受理しつつ即 501 (何も書かない)。

## 2. TDD (`tools/tests/test_host_agent.py` を拡張、Python 単体)
既存の贋 OS32 (フレームを直接組む) で:
- PRINT OPEN → `job 1`、DATA (宣言長 = WDATA 長) を 2 回追記 → スプールに連結、CLOSE → `pages n` + スプール内容一致。
- **宣言長と WDATA 長の不一致** (宣言 > 実 WDATA、宣言 < 実) の扱い (N1 の WDATA 契約に従う = Agent は宣言長ぶん受けてから応答)。
- 未知 id / CLOSE 済み id への DATA = 409、STATUS の各状態遷移 (open→closing→done、error)。
- CLIP PUT (宣言長 + WDATA) → `--clip-file` に書かれる、CLIP GET → `--clip-file` の内容を返す (往復)。4KB 超の CLIP PUT = 400。
- `--to-file` の CLOSE がスプールを残す / `--print-dir` へ移す。pywin32 が無い環境で全部通る (win32 系は import 失敗を許容)。
- **B1**: 完了後 / 400 後の WDATA seq を再送 → ACK(`ack=N`) が返り、スプールが二重に伸びず RESPONSE も再送されない (冪等)。最終 WDATA 再送でスプール不変。
- **B2**: `PRINT DATA 1 0` / `CLIP PUT 0` / `PRINT DATA 1 65537` / `CLIP PUT 4097` / `PRINT DATA 1 +5` → 業務 400 が WDATA 無しで即返る。
- **B3**: 409 / 500 / 503 に本文を付けた応答を FakeOS32 が WINDOW→DATA/EOF で読み切る。既存 `GET /status/410` の `gone body` を実際に読む。
- **B4**: state_dir を共有した Agent を 2 回起動し各 1 ジョブ印刷 → 出力ファイルが 2 つ両方残る (名前衝突なし)。
- **B5**: `--clip none` で GET/PUT が 503。`--clip file:<p>` で往復一致。`--clip wsl` は `clip.exe`/`powershell` を subprocess スタブに差し替えて 1 本。
- **B6**: `PUT /file/x 3` + WDATA 3B → 501、ファイルは作られない。
- **並行 / 冪等**: 2 ジョブ同時 open で交互 DATA が正しいスプールへ。重複 REQUEST / CLOSE の RESPONSE 消失 → STATUS 再提示で再印刷しない。
- 既存 25 ケース (HELLO / 台帳 / 墓標 / 枯渇) に回帰なし。`HostAgent.__init__` に試験用引数 (`spool_dir`/`print_dir`/`clip`/`printer`)。

## 3. 受入 (PM)
`python3 -B tools/tests/test_host_agent.py` 全通過。ゲスト受入は N3 (`lpr`/`hclip` 実装後) で実サービスとして: 端末から `lpr /etc/profile` がホストの spool/print-dir に落ちる、`hclip` の往復。N2 単体はホスト Python 試験まで。

## 4. レビューで見てほしい点 (設計)
1. ジョブ表と `_finish_body` の分岐が N1 の WDATA 契約 (宣言長ぶん受けて 200) を壊さないか。
2. pywin32 を任意依存にし `--clip-file` / `--to-file` で pywin32 無しでも完結する設計が妥当か。
3. 409 / 400 / 5xx の返し方が業務結果 (RESPONSE flags 0) で、N1 の制御結果 (flags bit0) と混ざらないか。
4. スプールの並行 (複数ジョブ同時 open、同一ジョブへの並行 DATA の順序、CLOSE と後着 DATA の競合) と、再起動をまたぐ成果物名の一意性 (B4)。
5. B1 の「完了後も ACK」がカーネルの再送を止めるのに十分か (カーネル `link_on_response` が保留 WDATA を無効化しない N1 潜在バグは別記 — B1 の Agent 側修正で実害は消えるが、将来の N-fix 候補として §5 に残す)。
6. CLIP の WSL2 バックエンド (`clip.exe` / `powershell`) の文字コード (UTF-16LE) と、それでも実クリップボードは N2 の受入外 (試験は file バックエンド) でよいか。

## 5. N1 の潜在バグ (N2 では触らない、将来の N-fix 候補)
`net/link.c` の `link_on_response` は業務 RESPONSE 到着時に保留中の WDATA (`wlen`/`wacked`) を無効化しない。そのため最終 WDATA の ACK が落ちると、RESPONSE 受信後もカーネルが WDATA を RTO 再送し続ける。**B1 で Agent が完了後も累積 ACK を返すことで実害 (再同期) は消える**が、根治は「RESPONSE 着後は同 rid の WDATA 再送を止める」カーネル側の 1 行。N3 着手時にホスト TDD で踏んで判断する。
