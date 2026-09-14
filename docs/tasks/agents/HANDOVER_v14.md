# v1.4 引き継ぎ — 実装 PM (別エージェント) への入口

発行: Claude Code (設計者 / レビュアー、2026-09-14)。状態: **執筆中 — 仕掛かりが緑になるたびに更新する**。
体制の正典は [ROLES.md](ROLES.md) §0。この文書は「今どこにいて、次に何をするか、踏むと痛い所」だけを書く。

## 1. 今どこにいるか

| 領域 | 状態 (2026-09-14) | 根拠 |
|---|---|---|
| v1.3 | 全項目受入済み、main にマージ済み (`fac0d89`)。残件の小物 4 件 (§3) は Claude Code が処理中 | `docs/ROADMAP.md` v1.3、`docs/tasks/gui/v13/PLAN.md` |
| Host Services N0 (設計) | 完了 = `docs/tasks/network/TASK_N0.md` **第 5 版** (Codex 4 往復、最後は 4 件残したまま決裁 b で N1 へ。残った疑いは N1 のホスト TDD が踏む) | TASK_N0 §7 |
| N1 (ワイヤ v2 / link.c / KAPI v51 / Agent v2) | **着地済み** (`995bb19` + `f5dca53`)、ホスト試験 54 ケースは worktree で緑。`make all` / `make check` / `make external` はテスター実行中 (結果は本書 §1 を更新)。**ゲスト受入は未実施** | `docs/tasks/network/TASK_N1.md`、`tools/tests/n1_tdd.md` |
| v1.4 の範囲 | ROADMAP §1 v1.4 = N1〜N4、R2 (PEGC / Cirrus 8bpp)、About、GUI エディタ。アプリ群は v2.0 以降へ | `docs/ROADMAP.md` (`23a28be`) |

## 2. 次にやること (順)

1. **N1 のゲスト受入** (TASK_N1 §2): `kernel-lgy98-link` を配備 ([D1] NP21/W 停止 → `nhd-pull` → `os32-cycle deploy` → `make deploy` → 起動)、WSL2 で `python3 tools/host_agent.py --state-dir <dir>` (v2、NP2NETSOCK の向きは `tools/host_agent.py` 冒頭)、ゲストで `host_test`、`check-net-l0`〜`l3` と `check-net-m2` の回帰、GUI 配下で `gui_busy` と同時に `host_test`。結果は TASK_N1 §3 に。
2. **N1 の実装レビュー**: Claude Code (設計者 / レビュアー) に材料 (差分の commit 範囲、n1_tdd.md、受入の観測) を渡す。Codex が使えるなら併用 (ROLES §5 の網羅指示)。
3. N2 (Agent の PRINT / CLIP / PUT)、N3 (`libos32host` + `wget` / `lpr` / `hclip` / `date -sync`)、N4 (GUI) — **票の設計は Claude Code が書く**。実装 PM は票が「設計レビュー通過」になってから発注する。
4. R2、About、GUI エディタ — 同上。

## 3. v1.3 残件の小物 (Claude Code が緑にしてから渡す)

| 件 | 内容 | 状態 |
|---|---|---|
| タスクバー経路 | `button_up_on_taskbar_*` をパネル非依存に書き直し、T5b 撤去で消えた 4 本の被覆を戻す (gshell ホスト試験) | コーダー実行中 |
| `stat` | `userland/cmds/stat.c` (st_dev の decode、st_ino) | コーダー実行中 |
| S6 `tar` | microtar を vendor、`tar c|x|t` | コーダー実行中 |
| 試験の棚卸し | `docs/tasks/TEST_INVENTORY_2026-09-14.md` (再考の材料、実装ではない) | 分析エージェント実行中 |

小物ではないので**保留** (ユーザーの再考待ち): F3a〜c (SQLite VFS の正直化 / lock 表 / open フラグ)、F2c、FEP_BOUNDARY、MEMORY_RAM_INTEGRATION、DEVICE_RESERVATION (`docs/tasks/settings/S0_PLAN_2026-09-13.md` の後回し欄)。

## 4. 踏むと痛い所 (v1.3 で実際に踏んだもの)

- **PM はビルド・`make check`・配備を自分で回さない** (テスターへ。`tools/emu_agent/agent.py run "<英語のタスク>" --quiet --session <name>`、合否は `tools/emu_agent/logs/<session>/steps.jsonl` の `obs` で判断)。テスターの `make` は `MAKE_TARGETS` に登録された名前だけ通る (新しい `check-*` を足したら `agent.py` にも足す)。
- **NHD 配備の順序**: taskkill → `nhd-pull` (NP21/W が動いていると Permission denied) → `os32-cycle deploy` → `make deploy` (HostDrv) → 起動 → kselftest。`hsync` の前に `make deploy` (古いカーネルに戻った前例)。
- **FDD ブート中に `make all` をしない** (NP21W_DIR の `os32_boot.d88` を上書きする)。HDD ブートかは `ls /hd0` が無いこと + `/boot/vmkernel.lz4` があることで見る。
- **`/api/cmd` は POST body** (`curl -X POST .../api/cmd --data-binary "ver"`)。対話プログラムは ESC + `/api/key` + tvram polling (`scratchpad/fdd_run2.py` の方式)、`ime on` 中は `SHIFT+SPACE` を先に。255B 超の引数は rshell の行が壊れる。
- **worktree の差分は基点 commit と比較して着地** (`git diff --cached <基点>`、feat/gui HEAD と比べると他レーンのファイルが削除として混入)。着地後に D が無いこと、conflict マーカーが無いことを全ファイルで確認。
- **`.inc` を変えても `.bin` が再ビルドされない** (userland の依存に無い) → `build/programs.mk` に明示依存をパターン規則の**後**に置く。
- **KAPI を変えたら `make clean` → `make all` → `make external`** ([ABI3])。`kapi/` から `net/` のヘッダを include するなら `build/config.mk` の `INC_KAPI` (N1 で `-Inet` を足した)。
- **Codex レビューは 3 往復まで、解決しなければユーザーへ** (ROLES §5)。判定の見出しは `## 判定` を `rfind` で拾う。網羅指示 (「今回の往復で見つかる到達可能な欠陥をすべて挙げる、経路ごとに見た / 見ていない」) を毎回入れる。
- **設定の読み書きは OS 経由だけ** (S2 決裁): アプリは `os32gui_cfg_*` ラッパー、`app:` scope のみ。新しいアプリ (About / エディタ) もこれに従う。
- 新しい層を実装する票には**移植性調査** (`docs/tasks/portability/SURVEY_<票>.md`、観点は TASK_N1 §0 段 7) を含める (ユーザー指示 2026-09-14)。

## 5. 主要な入口

`CLAUDE.md` → `docs/INDEX.md` (正典表) → `docs/tasks/network/HOST_SERVICES_PLAN.md` (§7 票の順序、§9 決裁済みの既定) / `docs/tasks/network/TASK_N0.md` (契約) / `docs/tasks/network/TASK_N1.md` (実装と受入)。スキル: `os32-build-verify`、`os32-emu-debug`、`os32-kapi-add`、`os32-emu-config`、`os32-local-ai`。
