# エージェント運用体制 — 役割と起動

状態: **現行 (2026-09-09 発効)**。このファイルは**今の体制だけ**を書く。
過去の体制変更の経緯は [RETROSPECTIVE_2026-09-09.md](RETROSPECTIVE_2026-09-09.md) にあり、
このファイルからは辿らせない (入口を履歴で太らせないため)。

## 1. 4 役

| 役 | 実体 | できること | してはいけないこと |
|---|---|---|---|
| **PM** | Claude Code 対話セッション (`claude-fable-5-1`) | 分解・受入判定・git 操作・[D1]〜[D3] の承認取得・エミュレータ操作 | 実装。手を動かし始めたら分解に戻る |
| **コーダー** | Claude Code サブエージェント (`claude-opus-5`)、worktree 隔離 | 実装 + ホスト TDD (`tools/tests/`) | 配備・コミット・エミュレータ操作 |
| **レビュアー** | Codex CLI (`~/.local/bin/codex`) | 読み取り専用レビュー、所見の列挙 | コードを直す。書き込みサンドボックスで動く |
| **テスター** | ローカル AI (FastFlowLM, `127.0.0.1:52625`) | 実機操作・回帰の**実行** | 合否の判断。台本にない操作 |

## 2. 起動 (2026-09-09 に実行して確認した形)

```bash
# PM — 対話セッションで /model claude-fable-5-1
#      (modelUsage に claude-fable-5-1 が出ることで確認する)

# コーダー — Claude Code の Agent tool
#   subagent_type: 任意, model: "opus", isolation: "worktree"
#   worktree は .gitignore 済みの .claude/worktrees/ に作られる
#   CLI から直接叩く場合: claude -p --model claude-opus-5 "<指示>"

# レビュアー — 読み取り専用サンドボックス。-o で最終所見をファイルに落とす
codex exec -s read-only -C /home/hight/os32 -o /tmp/review.txt "<レビュー指示>"

# テスター — 単発 / 定型回帰 / ログ確認
python3 tools/emu_agent/agent.py run "<タスク>"
python3 tools/emu_agent/agent.py suite tools/emu_agent/tasks/regress.txt
python3 tools/emu_agent/agent.py tail
```

厳格版が要る操作 (GUI 入力など、誤操作が実機を壊す経路) は `tools/emu_agent/playbook.py` を使う。
承認済み JSON 台本と操作・引数・順序が完全一致したものだけを実行し、既定は dry-run。

**FLM のモデルは、ユーザーがロード済みのものだけに投げる。**
`/v1/models` に並んでいることはロード済みを意味せず、未ロード名を投げると自動ダウンロードが始まる。

## 3. 規約 (3 行)

1. **PM が止まってよいのは 3 つだけ** — [D1]/[D2]/[D3] の承認、仕様の分岐、スコープ拡大。
   それ以外は承認済みスコープを最後まで進める。各ステップの終わりに「次に進んでよいか」を訊かない。
2. **ブロックできるのは到達可能な反例を伴う所見だけ**。現契約の違反はブロック、将来要件は別ゲートに落とす。
   反例を示せない指摘で、正しいコードを変えない。誤検出はレビュー記録側を訂正する。
3. **エミュレータは同時に 1 オペレータ**。コーダーは触らない。共有ファイルの編集は PM が直列化する。

## 4. 合否の判定

テスターの `RESULT: ok` は**自己申告**として扱う。合否は PM が
`tools/emu_agent/logs/<session>/steps.jsonl` の `obs` を読んで決める。
配備の反映確認は [08_build.md §8-4](../../08_build.md#配備3経路) の経路と、
`os32-cycle deploy` のゲスト側サイズ照合による ([V1]/[V4])。
