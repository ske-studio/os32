# OS32 ドキュメント索引

PC-9801シリーズ向け 32ビット ベアメタルOS

---

## 情報単位ごとの正典 (更新先は 1 つ)

同じ事実を 2 か所で独立に更新する構造は必ず食い違う (2026-09-05 の診断で 6 件)。
**変わりやすい数値・手順・進捗は下表の正典だけを更新し、他の文書は要約と参照に留める。**

| 情報単位 | 正典 (ここだけ更新) | 参照側 (要約 + リンクのみ) |
|---|---|---|
| 制約規則 [C/HW/ABI/V/D] | [CONSTRAINTS.md](CONSTRAINTS.md) | CLAUDE.md / SOUL.md (ID 参照、`make check` が照合) |
| 引き継ぎ (次の PM への申し送り) | [tasks/agents/HANDOVER_2026-09-18.md](tasks/agents/HANDOVER_2026-09-18.md) (最新) | 前回は [tasks/agents/HANDOVER_2026-09-16.md](tasks/agents/HANDOVER_2026-09-16.md) |
| エージェント運用体制 (役割・起動・規約) | [tasks/agents/ROLES.md](tasks/agents/ROLES.md) (現行のみ) | CLAUDE.md (4 行 + リンク)。過去の経緯は [tasks/agents/RETROSPECTIVE_2026-09-09.md](tasks/agents/RETROSPECTIVE_2026-09-09.md) 側に置き、入口からは辿らせない |
| 番地・帯域 | `include/memmap.h` (定義) → [02_memory.md §2-1](02_memory.md) の**生成ブロック** (`tools/gen_memmap.py --write`、地図はここ 1 か所だけ) | CLAUDE.md は帯の粒度のみ。`memmap.h` の先頭は生成先への案内。重なり・逆転・写しのずれは `make check` の `gen_memmap.py --check` が見る |
| KAPI の一覧・オフセット・版 | `sdk/kapi.json` → [KAPI_SPEC.md §4](KAPI_SPEC.md) | README.md / このファイル / KAPI_SPEC.md の版番号 (`tools/check_kapi_version.py` が照合。CLAUDE.md は版数を持たない) |
| KAPI 追加手順 | [KAPI_SPEC.md §3-1](KAPI_SPEC.md) | スキル `.claude/skills/os32-kapi-add` と CLAUDE.md (どちらもポインタのみ) |
| KAPI 版番号・エラー番号の予約 (未実装の先取り調停) | [KAPI_SPEC.md §3-2](KAPI_SPEC.md) | 各計画 (GUI TASK_K1、network LINK_PLAN) は参照 |
| エラーコード | `os32_kapi_shared.h` の `OS32_ERR_*` | 各 FS は境界で翻訳 |
| GUI 共有プロトコル (op / イベント / 構造体 / SHM 配置) | `sdk/include/os32/os32_gui_shared.h` (C が正典) → [tasks/gui/API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md) (契約) | `sdk/rust/os32api/src/gui/proto.rs` は写し (`tools/check_gui_proto.py` = `make check-gui-proto` が照合)、[tasks/gui/PROTO_LAYOUT.md](tasks/gui/PROTO_LAYOUT.md) |
| ビルドターゲット・ツール・コンパイラフラグ | [08_build.md](08_build.md) (フラグの実体は `build/config.mk`) | CLAUDE.md「Build Commands」(日常分のみ) |
| 配備 3 経路の使い分け | [08_build.md §8-4](08_build.md#配備3経路) | [POLICY_DEV.md §4](POLICY_DEV.md) (表のみ)、CLAUDE.md (1 行)、スキル `os32-build-verify` |
| ディレクトリ木 | [08_build.md §8-3](08_build.md) | CLAUDE.md / INDEX は参照のみ |
| ファイル → 役割 → 仕様の対応 | [DEVELOPMENT.md §2](DEVELOPMENT.md) | — |
| 作業別の参照先 | [DEVELOPMENT.md §1](DEVELOPMENT.md) | — |
| 実行モデル (ローダ、ネスト、リング3、資源回収、exec_run の分割壁) | [09_exec.md](09_exec.md) | [10 §10-9](10_notes.md)、`archive/kernel_v2/` (設計経緯) |
| 描画方式 (ページフリップ、200 ライン) | [05_drivers.md §5-5](05_drivers.md) | CLAUDE.md「Graphics」(1 行) |
| 落とし穴の経緯・検証記録 | [POLICY_DEBUG.md §4](POLICY_DEBUG.md) | CLAUDE.md「Known Gotchas」(2〜3 行の注意 + §番号) |
| コーディング規約 (C89、kstring、三層定数、asm) | [POLICY_DEV.md §2](POLICY_DEV.md) | CONSTRAINTS [C1]〜[C4] (規則行) |
| 進捗 | 領域別索引 ([tasks/fep/00_INDEX.md](tasks/fep/00_INDEX.md) の表、[tasks/v86v2/04](tasks/v86v2/04_implementation_status.md)、[tasks/gui/TASKS.md](tasks/gui/TASKS.md) のゲート) | [ROADMAP.md](ROADMAP.md) (計画)、[CHANGELOG.md](../CHANGELOG.md) (履歴) |
| プログラムの一覧 | 各層の `deploy.yaml` (機械可読の正典)、コマンドは [07_shell.md §7-1](07_shell.md) | 09_exec / INDEX に表を持たない |
| LAN の設計・進捗 | ドライバ = [tasks/network/PLAN.md](tasks/network/PLAN.md)、リンク層と Host Services = [tasks/network/LINK_PLAN.md](tasks/network/LINK_PLAN.md) | 05_drivers / DEVELOPMENT は要約 + リンク |
| 設定の置き場 (system.cfg の残すキー、settings.db のスキーマ / API / リカバリ) | [tasks/settings/DESIGN.md](tasks/settings/DESIGN.md) (計画、v1.3) | ROADMAP は 1 行 |
| アプリ帯の広さ (1 アプリに渡せる量) | [tasks/memory/APP_BAND_PDE.md](tasks/memory/APP_BAND_PDE.md) (実装済み `b8dab24`、kselftest で毎起動検証、K5b が依存。**票 §5 の受入項目は未消化** = 受入待ち) | 02_memory.md は方針と帯の表 |
| 試験の一覧 (`make check` のターゲット、`_tdd.md` と票の対応) | [TESTS.md](TESTS.md) (`tools/gen_tests_inventory.py` で生成、`make check-tests-inventory` が鮮度を照合) | 各票は自分の `_tdd.md` を指すだけ |
| 移植性 (CPU / 機種の 2 軸、ARM 計測、順序 1〜4 の経過) | [tasks/portability/ARM_GAUGE.md](tasks/portability/ARM_GAUGE.md) (計測と経過)、[../arch/README.md](../arch/README.md) (足し方) | [tasks/portability/SURVEY_N1.md](tasks/portability/SURVEY_N1.md) (調査)、`tasks/arch_port/` は**別リポジトリ `pw-sh4-research` の調査の快照** (正典はそちら。本リポジトリでは更新しない)、[tasks/portability/TASK_KSTRING_BENCH.md](tasks/portability/TASK_KSTRING_BENCH.md) (kstring の速度実測 **完了 2026-09-17** — x86 は asm 維持、C 版は他 32 ビットアーキ向け。数字は ARM_GAUGE §9、語長の前提は §10) |
| 版数 (カーネル 2.0 / GUI 1.x / 次期 v3 / v4 草案) | [ROADMAP.md §0](ROADMAP.md) | CHANGELOG.md、`ver` の文字列、タグ |
| 現行 / 未実装 / 過去 の区別 | 各文書の冒頭に「現行仕様」「計画」「YYYY-MM-DD 時点のスナップショット」を明記 | — |

## カーネル技術仕様書 (§1-§10)

| ファイル | 内容 |
|---------|------|
| [01_system.md](01_system.md) | **§1** システム概要 — アーキテクチャ、ブートシーケンス、レイヤー構造 |
| [02_memory.md](02_memory.md) | **§2** メモリマップ — 物理メモリ配置、DMA制約、ガードページ |
| [03_disk.md](03_disk.md) | **§3** ディスクレイアウト — FDD/HDD仕様、セクタ配置、INT 1Bh |
| [04_interrupts.md](04_interrupts.md) | **§4** 割り込みシステム — IDT/PIC/PIT |
| [05_drivers.md](05_drivers.md) | **§5** デバイスドライバ — KBD/Serial/FM/FDD/GFX/RTC/KCG/NP2SysP/libos32gfx |
| [06_filesystem.md](06_filesystem.md) | **§6** ファイルシステム — VFS/ext2/IDE/FDリダイレクト/パイプ |
| [07_shell.md](07_shell.md) | **§7** シェル — コマンド一覧、入力機能、スクリプトエンジン |
| [08_build.md](08_build.md) | **§8** ビルドシステム — パイプライン、ディレクトリ構造、デプロイツール |
| [09_exec.md](09_exec.md) | **§9** 外部プログラム実行 — OS32X/exec、ネスト実行、ステータスコード |
| [10_notes.md](10_notes.md) | **§10** 既知の制約と注意事項 |

## API・ガイド・ポリシー

| ファイル | 内容 |
|---------|------|
| [CONSTRAINTS.md](CONSTRAINTS.md) | **プロジェクト制約の正典** — C/ABI・ハードウェア・KernelAPI・検証・破壊的操作。CLAUDE.md と SOUL.md は ここの規則行を ID で参照する (`make check` が照合) |
| [POLICY_DEV.md](POLICY_DEV.md) | **開発ポリシー** — コーディング規約、ビルド/デプロイ、Gitコミット、テスト、リリース |
| [POLICY_DEBUG.md](POLICY_DEBUG.md) | **デバッグポリシー** — 仮説駆動デバッグ、バイナリ反映確認、教訓集、AI協調ルール |
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v56 仕様書 — 222エントリテーブル (ヘッダ2 + 関数218 + データフィールド2) + API追加手順 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | **開発案内** — 作業別の参照先 (読む / 触る / 検証) と、ファイル → 役割 → 仕様のファイル地図。仕様本文は持たない |
| [ROADMAP.md](ROADMAP.md) | リリースロードマップ (v1.0以降および履歴) |
| [archive/README.md](archive/README.md) | **アーカイブの運用** — 受入完了した票をどこへどう移すか (`tools/move_docs.py`)、移したあとも守ること。籠の一覧は「ログ」節 |
| [NHD_FORMAT.md](NHD_FORMAT.md) | NHD r0形式ファイル構造仕様 |
| [MGX_FORMAT.md](MGX_FORMAT.md) | MGX 漫画専用グレースケール画像形式 仕様 (48Bヘッダ + パレット表 + deflate、4bpp 16階調、ホスト側エンコード専用) |
| [BENCHMARK.md](BENCHMARK.md) | ベンチマークプログラム(bench.bin) の仕様とテスト内容 |

## プロジェクトルート

| ファイル | 内容 |
|---------|------|
| [LICENSE](../LICENSE) | MIT License (著作者: すけさん) |
| [README.md](../README.md) | プロジェクト概要・機能一覧・クイックスタート |
| [INSTALL.md](../INSTALL.md) | インストール・ビルド手順 |
| [CHANGELOG.md](../CHANGELOG.md) | リリース変更履歴 |

## ハードウェア技術資料 (外部リファレンス)

ハードウェアリファレンスはリポジトリ外の `C:\WATCOM\docs` (WSL: `/mnt/c/WATCOM/docs`) に配置されている:

| ドキュメント | 内容 |
|-------------|------|
| `C:\WATCOM\docs\undocumented\` | **非公開メモリ・I/Oポート資料集 (独自調査基盤、より正確)** |
| `C:\WATCOM\docs\PC9800Bible\` | PC-9800シリーズ テクニカルデータブック (公式資料ベース) |
| `docs/hw/` (git 管理外) | 上記と UNDOCUMENTED の Markdown をローカルにミラーしたもの。`tools/sync_hwdocs.sh` で更新。著作権物なので git に入れない |
| `C:\WATCOM\docs\D88_FORMAT_SPEC.md` | D88 フロッピーイメージ形式仕様 |
| `C:\WATCOM\docs\NP21W_DEBUG_PORT.md` | NP21/W デバッグポート資料 |

> **注意:** PC9800Bible と UNDOCUMENTED の記述が矛盾する場合は、UNDOCUMENTED の方を優先してください。

## ログ (歴史的記録)

| ドキュメント | 内容 |
|-------------|------|
| [PM_PIO_TEST.md](logs/PM_PIO_TEST.md) | プロテクトモード IDE PIO 読み込み実証実験記録 |
| [HDD_BIOS_DEBUG.md](logs/HDD_BIOS_DEBUG.md) | HDD ブート開発・デバッグログ（INT 1Bh / ディスクレイアウト） |
| [archive/REFACTORING_PLAN.md](archive/REFACTORING_PLAN.md) / [archive/ROADMAP_v1.0.md](archive/ROADMAP_v1.0.md) | 初期のリファクタ計画 / v1.0 到達までのロードマップ |

### archive/ — 受入完了した票 (運用は [archive/README.md](archive/README.md))

領域ごとの籠。中身は下の「タスク」の各小節からも同じ票を指している (**索引からは消さない**)。

| 籠 | 何の票か | 完了 |
|---|---|---|
| [archive/gui_v11/](archive/gui_v11/TASK_H1_hal_backend.md) | GUI シェル v1.1 の票 12 本 (H1〜H3 / K1〜K4 / W1 W2 / C1〜C3)。索引は [tasks/gui/TASKS.md](tasks/gui/TASKS.md) | 2026-09-06 |
| [archive/gui_v12/](archive/gui_v12/TASK_K5_v12_proto.md) | GUI シェル v1.2 の票 5 本 (K5 / W3 W4 / C4 C5)。索引は [tasks/gui/v12/TASKS.md](tasks/gui/v12/TASKS.md) | 2026-09-07 (`d739494`) |
| [archive/gui_v13_reviews/](archive/gui_v13_reviews/REVIEW_T0_T1.md) | GUI シェル v1.3 のレビュー記録 5 本 + [限定クロスリンク](archive/gui_v13_reviews/CROSS_LINK_T4.md) + [途中保存](archive/gui_v13_reviews/VERIFICATION_PROGRESS.md)。票そのものは `tasks/gui/v13/` に残る | 2026-09-14 |
| [archive/settings/](archive/settings/TASK_S2.md) | 設定レジストリの票 5 本 (S2 / S3 / S3I2 / S4 / S5)。S0 / S6 / S6P と設計文書は `tasks/settings/` に残る | 2026-09-13〜14 |
| [archive/network/](archive/network/TASK_N0.md) | Host Services の票 5 本 (N0〜N4)。計画 3 本は `tasks/network/` に残る | 2026-09-14〜15 |
| [archive/kernel_v2/](archive/kernel_v2/PLAN.md) | カーネル 2.0 の完了記録 8 本 (計画・M1〜M3・凍結契約・コーダー票 3 本)。タグ `v2.0` | 2026-09-03 |
| [archive/TEST_INVENTORY_2026-09-14.md](archive/TEST_INVENTORY_2026-09-14.md) | 試験の棚卸しの快照。正典は [TESTS.md](TESTS.md) へ移行 | 2026-09-14 |
| [archive/debug_kcg_load_font.md](archive/debug_kcg_load_font.md) | `kcg_load_font` クラッシュの仮説計画 (単発の障害記録) | — |

## タスク

領域ごとに、**計画 → 票 → 記録**の順。票の冒頭の `状態:` 行が正典 (語彙: 計画 / 設計中 / 実装中 / 受入待ち / 受入完了 / 撤回 / 完了記録)。
受入完了して参照頻度が下がった票は `docs/archive/<領域>/` へ落とすが、**索引の行は残す** (リンク先が archive になるだけ)。
移し方と運用は [archive/README.md](archive/README.md)。

### シェル・配備 (hsync)

| ドキュメント | 内容 |
|-------------|------|
| [tasks/shell/HSYNC_IMPROVEMENT_PLAN.md](tasks/shell/HSYNC_IMPROVEMENT_PLAN.md) | hsync 改善案 (ユーザー起草、2026-09-14) — H1 / H3 は受入完了、H2 (置換の安全化) / H4 (配備マニフェスト) は未着手 |
| [tasks/shell/TASK_H1.md](tasks/shell/TASK_H1.md) | H1 **受入完了 (2026-09-15)** — 同サイズ内容比較、ストリーム CRC + 読戻し検証、dry-run、理由表示、HostDrv stat の是正。Codex 往復 5 の記録 |
| [tasks/shell/TASK_H3.md](tasks/shell/TASK_H3.md) | H3 **受入完了 (2026-09-15)** — HostDrv の FILETIME→mtime、`sys_set_mtime` (KAPI v52)、日時を前置フィルタに (決裁)。`hsync sys` 25.8 s → 0.26 s |
| [tasks/shell/TASK_H2.md](tasks/shell/TASK_H2.md) | H2 **受入完了 (2026-09-16)** — hsync の置換安全化。`O_EXCL` (KAPI v53)、ext2 のファイル置き換えを宛先エントリの inode 書き換えに、一時ファイル `.hs~` → 検証 → rename。決裁 D1〜D3 |
| [tasks/shell/TASK_H4.md](tasks/shell/TASK_H4.md) | H4 **受入完了 (2026-09-16)** — 配備マニフェストと世代の確認。古い配備元で新しい成果物を上書きする事故を検出する。`--expect-build` と行指向の名札 |
| [tasks/shell/TASK_FS_TYPE.md](tasks/shell/TASK_FS_TYPE.md) | B8 **受入完了 (2026-09-15、6 往復)** — 読み取り失敗を不存在・未割当・別の型と読み替えていた ext2/VFS/HostDrv の経路。remount-ro 相当、e2fsck を正解に。残る制限は §2-6 / §2-7 |
| [tasks/shell/TASK_SH_TRUNCATION.md](tasks/shell/TASK_SH_TRUNCATION.md) | シェルの入力切り詰め **受入完了 (2026-09-16)** — 切り詰めたまま実行を続ける 26 経路。`if` の比較が 255 文字で切れて条件が逆転し破壊的なコマンドが走る欠陥を含む。`$?` の配線より先 |
| [tasks/shell/TASK_EXIT_STATUS.md](tasks/shell/TASK_EXIT_STATUS.md) | 終了コードの配線と `$?` **受入完了 (2026-09-16)** — ゲスト試験ランナーの 1 段目 (KAPI v55、`exec_last_result`)。終了コードが起動エラー・app_id と同じ空間に混ざっている (PATH の次の候補を二重実行する実害つき)。決裁 E1 / E2 |
| [tasks/tools/TASK_KEY_INJECT.md](tasks/tools/TASK_KEY_INJECT.md) | キー注入で任意のバイトを送る **受入完了 (2026-09-18)** — `/api/key` の `text=ABC` が `abc` になる (変換関数が小文字に畳んでから needshift を 0 に固定)。**検証の側の穴**で OS32 本体は変えない。直す先は NP21/W と gui_gate.py。エディタの受入 E2〜E7 がこれで止まっている |
| [tasks/gui/TASK_EDIT_GUI.md](tasks/gui/TASK_EDIT_GUI.md) | テキストエディタの GUI 版 **計画 (2026-09-17)** — v1.4 の最後の受入試験。アプリを増やすのが目的ではなく **API の退行検出を兼ねる**。中心は**複数行の編集部品** (既存の textbox は 1 行しか扱えない)。libos32gui / 設定 / Host Services を通しで使う |
| [tasks/v3/PLAN.md](tasks/v3/PLAN.md) | **v3 の計画 (2026-09-17)** — 機能を足す前に入れ物を作り直す。順序は C11 → メモリマップ再配置 → **ドライバの動的読み込み** → PCI → Intel 82557。カーネルは 433.6KB で残り 34.4KB しかなく、静的リンクのままでは積めない。アプリへの払い出しの見直しは §4。**アイデア (計画ではない) は §5** — ネットワーク越しの仮想メモリ、**V86 の装置要求をホストへ逃がす** (86 ボードを実機に用意せず互換性を出す)、EMS をネットワークへ、レガシー VRAM をホストへ転送 (実機に画面を取る手段が無い穴を埋める) |
| [tasks/realhw/PLAN.md](tasks/realhw/PLAN.md) | 実機 PC-9821Ra266 で動かす計画 **計画 (2026-09-17、着手は v3)** — 8GB ディスクは CHS 専用ドライバで未検証、FD 1.2MB と CD は実装済み、**ホスト側にシリアルの実装が無いのが最大の穴**、LAN は C バスの LGY-98 が最短、PCI と Trident は v3 |
| [tasks/memory/TASK_KSTACK_USER.md](tasks/memory/TASK_KSTACK_USER.md) | **SHM 帯がカーネルスタックに食い込んでいる 受入完了 (2026-09-17)** — 番地が `__bss_end` から浮くため、カーネルが育って SHM の終端がスタックに達した。**スタックは設計の 16KB ではなく 4KB**。設計上のガードは存在せず `crash` が素通りする。重なりを誰も検査していない。**POLICY_DEV §1 で最優先** |
| [tasks/sqlite/TASK_DB_ERRSTR.md](tasks/sqlite/TASK_DB_ERRSTR.md) | `db_last_error()` がカーネル番地を返す **受入完了 (2026-09-17)** — CPL=3 のアプリが戻り値を読むと #PF で死ぬ (実測 addr=0x002B8DE0、`$?`=139)。`db_column_text()` のエラー経路も同じ。共有メモリへ写して返す。**カーネル層なので POLICY_DEV §1 で優先** |
| [tasks/test/TASK_FSTAT_REDIR.md](tasks/test/TASK_FSTAT_REDIR.md) | **`fstat` がリダイレクトを見ない 受入完了 (2026-09-17)** — fd 0/1/2 を無条件で キャラクタデバイスと答えるので `isatty` と食い違う。`stat_t` が単独では 5/5、リダイレクトすると 4/5。**POLICY_DEV §1 でカーネル層が先** |
| [tasks/test/TASK_TEST_RUNNER.md](tasks/test/TASK_TEST_RUNNER.md) | ゲストで一括実行してホストで集計する **受入完了 (2026-09-17)** — ランナー 3 段目。シェルに `;` もループも無いのでホストが平らな スクリプトを生成する。終了コードと集計行の**両方**を突き合わせ、食い違いを不合格にする。固まった試験を名指しする見張りつき。`make check-guest` として `make check` とは別 |
| [tasks/test/TASK_TEST_RESULT.md](tasks/test/TASK_TEST_RESULT.md) | 合否を機械が読める形にする **受入完了 (2026-09-17)** — ゲスト試験ランナーの 2 段目。終了コード 0/1/2 と集計行 `<名前>: PASS n/m` の制定、結果チャネルは HostDrv (決裁 R1)。`void main` 20 本と、回帰台本の偽合格 (`alloc_demo`)、`ring3_fault` / `ring3_hello` の古い `int 0x80` 規約も直す |
| [tasks/shell/INHERITED_BUGS.md](tasks/shell/INHERITED_BUGS.md) | 継承バグ台帳 (T9 で起こした、常駐シェルと sh.bin の共通) |

### 設定レジストリ (settings、v1.3 で完了)

完了した票 (S2 / S3 / S3I2 / S4 / S5) は [archive/settings/](archive/settings/TASK_S2.md) へ。

| ドキュメント | 内容 |
|-------------|------|
| [tasks/settings/DESIGN.md](tasks/settings/DESIGN.md) | **設定レジストリ**の設計 — `system.cfg` (起動キー) + `/etc/settings.db` (SQLite)。置き場の正典 |
| [tasks/settings/S0_PLAN_2026-09-13.md](tasks/settings/S0_PLAN_2026-09-13.md) | 着手計画 (PM 縮約案、2026-09-13 決裁) |
| [tasks/settings/S0_FOUNDATION.md](tasks/settings/S0_FOUNDATION.md) | S0 の基盤設計 (配備の保護、所有権) |
| [tasks/settings/TASK_S0.md](tasks/settings/TASK_S0.md) / [TASK_S2.md](archive/settings/TASK_S2.md) / [TASK_S3.md](archive/settings/TASK_S3.md) / [TASK_S3I2.md](archive/settings/TASK_S3I2.md) / [TASK_S4.md](archive/settings/TASK_S4.md) / [TASK_S5.md](archive/settings/TASK_S5.md) / [TASK_S6.md](tasks/settings/TASK_S6.md) / [TASK_S6P.md](tasks/settings/TASK_S6P.md) | 票 S0〜S6P (いずれも受入完了、2026-09-13〜14) — KAPI v50 db_*、libos32cfg と `cfg`、install の回復、gshell の消費者、実測、tar |
| [tasks/settings/F2_OWNERSHIP.md](tasks/settings/F2_OWNERSHIP.md) / [FEP_BOUNDARY.md](tasks/settings/FEP_BOUNDARY.md) / [DEVICE_RESERVATION.md](tasks/settings/DEVICE_RESERVATION.md) / [MEMORY_RAM_INTEGRATION.md](tasks/settings/MEMORY_RAM_INTEGRATION.md) | 設計提案 (F2 の scoped 実装以外は未着手) |

### ネットワーク・Host Services (v1.4)

完了した票 (N0〜N4) は [archive/network/](archive/network/TASK_N0.md) へ。計画 3 本はここに残る。

| ドキュメント | 内容 |
|-------------|------|
| [tasks/network/PLAN.md](tasks/network/PLAN.md) | LGY-98 / NE2000 **ドライバ**計画 — M1〜M3 エミュレータ合格、既定で有効 (2026-09-14 決裁) |
| [tasks/network/LINK_PLAN.md](tasks/network/LINK_PLAN.md) | リンクプロトコル (ワイヤ v2) / Host Services 計画 |
| [tasks/network/HOST_SERVICES_PLAN.md](tasks/network/HOST_SERVICES_PLAN.md) | Host Services 詳細計画 — N1〜N4 受入完了、N5 (実機) は保留 |
| [archive/network/TASK_N0.md](archive/network/TASK_N0.md) / [TASK_N1.md](archive/network/TASK_N1.md) / [TASK_N2.md](archive/network/TASK_N2.md) / [TASK_N3.md](archive/network/TASK_N3.md) / [TASK_N4.md](archive/network/TASK_N4.md) | 票 N0〜N4 (受入完了、2026-09-14〜15) — 設計 v5、KAPI v51 + libos32host、PRINT/CLIP、wget/lpr、ファイラ印刷と端末の貼り付け |

### 移植性

| ドキュメント | 内容 |
|-------------|------|
| [tasks/portability/ARM_GAUGE.md](tasks/portability/ARM_GAUGE.md) | **ARM コンパイル計測の基準値と経過** — `make check-arm-compile` (計測、合否ではない)。順序 1〜4 の前後表 (§9)。2026-09-15 時点 55/93 |
| [tasks/portability/SURVEY_N1.md](tasks/portability/SURVEY_N1.md) | 移植性調査 (N1 起点) — 直列化とアライメント、`cli`/`sti`/`hlt` の一覧、順序 2 / 4-a の実施記録 |
| [../arch/README.md](../arch/README.md) | **`arch/` と `platform/` の正典** — 移植の 2 軸 (CPU / 機種)、`ARCH` `PLATFORM` の選び方、新アーキテクチャの足し方 |
| [tasks/arch_port/00_INDEX.md](tasks/arch_port/00_INDEX.md) | 他アーキテクチャ移植調査の索引 — **別リポジトリ `pw-sh4-research` で進む調査の快照 (2026-09-08〜09、本リポジトリでは更新しない)**。M0 監査 (`tools/audit_cast_align.sh`)、SHARP Brain (i.MX28) のハード調査 |

### GUI シェル (v1.1〜v1.4)

完了した票は [archive/gui_v11/](archive/gui_v11/TASK_H1_hal_backend.md) (v1.1 の 12 本) /
[archive/gui_v12/](archive/gui_v12/TASK_K5_v12_proto.md) (v1.2 の 5 本) /
[archive/gui_v13_reviews/](archive/gui_v13_reviews/REVIEW_T0_T1.md) (v1.3 のレビュー記録) へ。
設計・契約・索引 (`DESIGN.md` `API_CONTRACTS.md` `TASKS.md` `PROTO_LAYOUT.md` と v1.3 の票) はここに残る。

| ドキュメント | 内容 |
|-------------|------|
| [tasks/gui/DESIGN.md](tasks/gui/DESIGN.md) | **GUI シェル v1.x 設計記録** (2026-09-04) — 再描画モデル、HAL/バックエンド表 |
| [tasks/gui/API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md) | libos32gui 凍結インターフェース契約 (2026-09-04 凍結) |
| [tasks/gui/TASKS.md](tasks/gui/TASKS.md) | v1.1 作業分担票とゲート表 — v1.1 の票 (`archive/gui_v11/TASK_*.md` 12 本) の索引 |
| `archive/gui_v12/` | v1.2 の票 5 本 (受入完了 2026-09-07 `d739494`)。索引は [tasks/gui/v12/TASKS.md](tasks/gui/v12/TASKS.md) |
| [tasks/gui/v13/PLAN.md](tasks/gui/v13/PLAN.md) | **v1.3 計画と票の索引** (受入完了 2026-09-14) — K5b / K6 / K7 / T7〜T9、監査 (`AUDIT_2026-09-10.md`)、レビュー記録 (`archive/gui_v13_reviews/REVIEW_*.md`、完了記録) |
| [tasks/gui/v13/TASK_K6C_A_terminal.md](tasks/gui/v13/TASK_K6C_A_terminal.md) / [TASK_T7_terminal_cmd.md](tasks/gui/v13/TASK_T7_terminal_cmd.md) / [REVIEW_T5A_APP.md](archive/gui_v13_reviews/REVIEW_T5A_APP.md) | v1.3 の票のうち `PLAN.md` から直接辿れない 3 本 (端末アプリ、端末からの CUI 起動、T5a アプリのレビュー記録) |
| [../tools/tests/gui_review_20260910_tdd.md](../tools/tests/gui_review_20260910_tdd.md) / [gui_review3_20260910_tdd.md](../tools/tests/gui_review3_20260910_tdd.md) | v1.3 レビュー往復 (2026-09-10) の試験記録 (完了記録) |
| [tasks/agents/HANDOVER_2026-09-16.md](tasks/agents/HANDOVER_2026-09-16.md) | **残件の引き継ぎ (2026-09-16、計画)** — 別モデルが PM として進めるための文書。現在地、残件 (H2 / H4 / arch 移設 / kstring 判断 / ゲスト試験ランナー / ARM / LAN 実機 / 小物) の推奨順・決裁点・手順・受入、踏むと痛い所 |
| [tasks/agents/HANDOVER_v14.md](tasks/agents/HANDOVER_v14.md) | v1.4 の引き継ぎ — **撤回 (2026-09-14)**。アプリ層を別エージェントへ渡す案は取りやめ、実装は基盤・アプリ層とも Opus 5 コーダー |
| [tasks/hotdeploy/DESIGN.md](tasks/hotdeploy/DESIGN.md) | ホットデプロイ (再起動なしの配備) の設計 |

### カーネル 2.0 (完了記録) と次期カーネル

2.0 の 8 本はすべて [archive/kernel_v2/](archive/kernel_v2/PLAN.md) へ (完了記録)。

| ドキュメント | 内容 |
|-------------|------|
| [archive/kernel_v2/PLAN.md](archive/kernel_v2/PLAN.md) | **カーネル 2.0 の計画 (完了記録)** — リング 3 / Rust の適用範囲 / KAPI 呼び出し実測。M1〜M3 は 2026-09-03 完了、タグ `v2.0`。版数の対応は [ROADMAP.md §0](ROADMAP.md) |
| [archive/kernel_v2/M1_RING3.md](archive/kernel_v2/M1_RING3.md) / [M2_KAPI_TRAMPOLINE.md](archive/kernel_v2/M2_KAPI_TRAMPOLINE.md) / [M3_VERIFY.md](archive/kernel_v2/M3_VERIFY.md) / [CONTRACTS.md](archive/kernel_v2/CONTRACTS.md) | 2.0 の設計 (リング 3 土台、KAPI トランポリン、検証、凍結契約)。完了記録 |
| [archive/kernel_v2/TASK_coder1_M0b_privileged.md](archive/kernel_v2/TASK_coder1_M0b_privileged.md) / [TASK_coder1_M1_ring3.md](archive/kernel_v2/TASK_coder1_M1_ring3.md) / [TASK_coder2_libos32gui.md](archive/kernel_v2/TASK_coder2_libos32gui.md) | 2.0 のコーダー票。完了記録 |
| [V4_GAME_PLATFORM_DRAFT.md](V4_GAME_PLATFORM_DRAFT.md) / [tasks/v4/README.md](tasks/v4/README.md) | ゲーム基盤 v4 の草案 (2026-09-07)。v3 (次期カーネル、未定義) の後 |
| [tasks/boot_reform/00_OVERVIEW.md](tasks/boot_reform/00_OVERVIEW.md) | ブート刷新 (vmkernel.lz4 / ext2 ローダー) — 設計 (全 8 部) |

### FEP・V86・SQLite・ライブラリ

| ドキュメント | 内容 |
|-------------|------|
| [tasks/fep/00_INDEX.md](tasks/fep/00_INDEX.md) | FEP (日本語入力) 拡張 — 詳細設計 P1〜P7 の索引 (実装状況付き) |
| [tasks/fep/FEP_STATUS.md](tasks/fep/FEP_STATUS.md) / [FEP_FUTURE.md](tasks/fep/FEP_FUTURE.md) | FEP のアーキテクチャ説明 (2026-04-27 の快照) / 今後の拡張 |
| [tasks/v86v2/README.md](tasks/v86v2/README.md) | **V86 サブシステム (再挑戦)** — 16bit ゲスト実行。進捗の正典は `04_implementation_status.md` |
| [tasks/wintree_port/PORT_PLAN.md](tasks/wintree_port/PORT_PLAN.md) | feat/vdm 系作業ツリーの移植計画と実施結果 |
| [tasks/sqlite/00_INDEX.md](tasks/sqlite/00_INDEX.md) | SQLite カーネル統合 — 設計・実装 (全 7 部) |
| [tasks/tilemap/00_INDEX.md](tasks/tilemap/00_INDEX.md) | タイルマップ / ブリット最適化 — 設計・最適化・TODO の索引 (全 8 部) |
| [tasks/libmath/LIBMATH_DESIGN.md](tasks/libmath/LIBMATH_DESIGN.md) / [tasks/libinput/LIBINPUT_DESIGN.md](tasks/libinput/LIBINPUT_DESIGN.md) / [tasks/libasset/LIBASSET_DESIGN.md](tasks/libasset/LIBASSET_DESIGN.md) / [tasks/libecs/LIBECS_DESIGN.md](tasks/libecs/LIBECS_DESIGN.md) / [tasks/libtext/LIBTEXT_DESIGN.md](tasks/libtext/LIBTEXT_DESIGN.md) | ライブラリ設計書 (math / input / asset / ecs / text) |
| `tasks/libai/` `libbattle/` `libboard/` `libecon/` `libevent/` `libinv/` `tilemap/` | 各ゲームライブラリの設計書群 (別リポジトリ `os32-game` に移った分は `os32-game:docs/...`) |
| `os32-game:docs/game/GAME_PORT_PLAN.md` / `os32-game:docs/game/ENGINE_EXTENSION_PLAN.md` / `os32-game:docs/libchem/LIBCHEM_DESIGN.md` | 対戦スゴロク RPG の移植・エンジン拡張・化学エンジン (別リポジトリ ske-studio/os32-game) |

### 単発の記録

| ドキュメント | 内容 |
|-------------|------|
| [TESTS.md](TESTS.md) | **試験の一覧** (正典、生成) — `make check` の全ターゲット、`_tdd.md` と票の対応、改善提言 |
| [archive/TEST_INVENTORY_2026-09-14.md](archive/TEST_INVENTORY_2026-09-14.md) | 試験の棚卸し (2026-09-14 の快照)。正典は `TESTS.md` へ移行 |
| [archive/debug_kcg_load_font.md](archive/debug_kcg_load_font.md) | `kcg_load_font` クラッシュの仮説計画 (単発の障害記録) |
| [tasks/cross_compiler_rebuild.md](tasks/cross_compiler_rebuild.md) / [tasks/ext2_dind_debug.md](tasks/ext2_dind_debug.md) | クロスコンパイラ再構築 / ext2 二重間接の障害記録 |

## man ページ

`docs/manpages/*.1` — ゲスト内 `man` コマンド用マニュアル (約60ページ)。
`make packages` で `/usr/man/` に配置される (`tools/package_defs.yaml`)。

## ソースツリー概要

[08_build.md §8-3](08_build.md) を参照 (複製しない。CLAUDE.md「Source Tree」も同じ表)。
