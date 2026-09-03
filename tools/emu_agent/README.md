# emu_agent — ローカル小型 LLM に OS32 実機を操作させる

`flm serve` (FastFlowLM, OpenAI 互換 API, `127.0.0.1:52625`) 上のモデルに
NP21/W (ai-debug 版) の HTTP API 経由で OS32 のシェルを触らせる最小ドライバ。

```
python3 tools/emu_agent/agent.py run "ver を実行してバージョンを報告して"
python3 tools/emu_agent/agent.py run "..." --model gemma4-it:e4b --max-steps 12
python3 tools/emu_agent/agent.py suite tools/emu_agent/tasks/regress.txt   # 定型回帰
python3 tools/emu_agent/agent.py suite tools/emu_agent/tasks/v86_dos.txt   # DOS 操作
python3 tools/emu_agent/agent.py tail            # 直近セッションのログ末尾
```

`suite` はタスクファイル (`#` コメント、空行区切りで 1 タスク) を順に実行し、
SUMMARY 表と全体の `RESULT:` を出す。

最後に `RESULT: {...}` を 1 行出す (`ok` / `report` / `steps` / `session`)。
ステップごとの生ログは `tools/emu_agent/logs/<session>/steps.jsonl`。

## 設計

- **ツール定義は system prompt の平文 + JSON 1 個の返答**。FLM のネイティブ
  function calling はモデルごとに挙動が違うので依存しない (`tool_calls` が
  返ってきた場合は受ける)。
- 実機側の行動は `cmd` (`/api/cmd`) / `key` (`/api/key`, 値は URL エンコードするので
  `CTRL+STOP` がそのまま書ける) / `tvram` / `status` / `done`、および
  `cmd_nowait` (V86 セッションなど返ってこないコマンドを投げっぱなし) /
  `wait` (最大 60 秒) / `screenshot` (`logs/<session>/shots/stepNN.bmp` に保存) /
  `selftest` (kernel.map で `kselftest_pass/fail` を引いて `/api/mem` を読む)。
- ホスト側の行動は **許可リスト方式** (自由なシェルは渡さない):
  `make` (target は kernel/programs/sdk/all/check/clean のみ) /
  `hotdeploy` (`make hotdeploy FILE=<repo 相対 .bin>`) /
  `deploy` (`os32-cycle deploy` = 停止→NHD 配備→起動→ver。[D1] を機構で守る)。
  ログは RESULT 行・error 行・末尾 15 行に絞って渡し、モデルには
  「解釈せず exit= と RESULT: を引用せよ」と指示している。
- 実機出力は 1600 文字で切って渡す (4B 級モデルの文脈を守るため)。
- HTTP 層は `tools/np21w_mcp/np21w_client.py` を流用 (WSL 直叩き → curl.exe
  フォールバック)。`/api/cmd` のタイムアウトは 90s ([V3])。

## 上位モデルのコンテキストを守る使い方

- `--quiet` (MCP 経由の既定) は `RESULT:` 1 行しか返さない。行動と観測は
  `steps.jsonl` にあるので、裏取りは `grep` で該当行だけ読む。
- 単発コマンド 1 つなら curl 直叩きの方が安い。委譲が効くのは「待ち」や
  多段の手順 (ブート → キー → 画面 → 脱出、ビルド → 配備 → 検証) を
  1 呼び出しに畳めるとき。

## 注意

- **未ロードのモデル名を渡すと flm が自動ダウンロードを始める。**
  `/v1/models` に並んでいるだけでは「取得済み」の保証にならない。
- FLM はリクエストを直列処理する。タイムアウトで切った側のリクエストも
  サーバ側では最後まで走るので、待たされたら数分置く。
- **chat が長く無応答なら flm serve 自体が落ちている可能性が高い**
  (`/v1/models` だけ返ることもある)。flm のコンソールを確認して再起動する。
- モデルの `report` は自己申告。合否の判断は `steps.jsonl` の観測 (`obs`)
  を人間/上位モデルが読んで行う。
- 2026-09-04 時点で `qwen3.5:9b` は FLM/NPU 上で記号の羅列しか出さない
  (ツール無しの平文でも同じ)。`gemma4-it:e4b` は正常。

## 委譲実績 (gemma4-it:e4b, 2026-09-04)

| タスク | ステップ |
|---|---|
| ver / ファイル作成→cat→ls / cd エラー→pwd / 画面読取 | 2〜5 |
| vdos.bin の所在調査 (/bin, /usr/bin, /sys, /host/bin) | 6 |
| `make check` | 2 |
| wc.bin の hotdeploy → `echo hello \| wc -c` で検証 | 3 |
| `os32-cycle deploy` → ver | 2 |
| DOS ブート (cmd_nowait) → dir → CTRL+STOP → ver (`tasks/v86_dos.txt`) | 11 |
| 定型回帰 6 本 (`tasks/regress.txt`: selftest / klibc_test / alloc_demo / ring3_fault / パイプ / screenshot) | 2〜3 ずつ |

## MCP

`tools/emu_agent/mcp_server.py` を `.mcp.json` に登録済み
(`emu_agent_run` / `emu_agent_suite` / `emu_agent_tail`)。ゲームの自動プレイは
os32-game 側の `tools/autoplay` (同じ flm/gemma 構成) で、`.mcp.json` の
`autoplay` エントリは `../os32-game/tools/autoplay/mcp_server.py` を指す
(隣に clone してある前提)。Claude Code から「ローカルモデルに
このタスクをやらせて結果を見る」ために使う。
