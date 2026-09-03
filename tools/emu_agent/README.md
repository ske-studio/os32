# emu_agent — ローカル小型 LLM に OS32 実機を操作させる

`flm serve` (FastFlowLM, OpenAI 互換 API, `127.0.0.1:52625`) 上のモデルに
NP21/W (ai-debug 版) の HTTP API 経由で OS32 のシェルを触らせる最小ドライバ。

```
python3 tools/emu_agent/agent.py run "ver を実行してバージョンを報告して"
python3 tools/emu_agent/agent.py run "..." --model gemma4-it:e4b --max-steps 12
python3 tools/emu_agent/agent.py tail            # 直近セッションのログ末尾
```

最後に `RESULT: {...}` を 1 行出す (`ok` / `report` / `steps` / `session`)。
ステップごとの生ログは `tools/emu_agent/logs/<session>/steps.jsonl`。

## 設計

- **ツール定義は system prompt の平文 + JSON 1 個の返答**。FLM のネイティブ
  function calling はモデルごとに挙動が違うので依存しない (`tool_calls` が
  返ってきた場合は受ける)。
- 行動は `cmd` (`/api/cmd`) / `key` (`/api/key`) / `tvram` / `status` / `done`。
- 実機出力は 1600 文字で切って渡す (4B 級モデルの文脈を守るため)。
- HTTP 層は `tools/np21w_mcp/np21w_client.py` を流用 (WSL 直叩き → curl.exe
  フォールバック)。`/api/cmd` のタイムアウトは 90s ([V3])。

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

## MCP

`tools/emu_agent/mcp_server.py` を `.mcp.json` に登録済み
(`emu_agent_run` / `emu_agent_tail`)。Claude Code から「ローカルモデルに
このタスクをやらせて結果を見る」ために使う。
