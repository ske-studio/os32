# docs/archive/ — 受入完了した票の置き場

**現行仕様 (運用の正典)。** 2026-09-16 開設。

`docs/tasks/` は「いま動いている作業」の棚であってほしいが、票は受入が済んでも消せない
(なぜその実装になったかの根拠であり、あとから仕様を問われるのはたいてい完了した票の中身)。
両方を満たすために、**受入完了して参照頻度が下がった票はここへ落とし、索引の行は残す**。
`docs/tasks/` を開いたときに目に入るのは進行中のものだけになり、完了した票は
`docs/INDEX.md` から 1 段深いところで生き続ける。

## 籠の切り方

領域 + 版で 1 つ。いま在るもの:

| 籠 | 中身 |
|---|---|
| `gui_v11/` | GUI シェル v1.1 の票 12 本 |
| `gui_v12/` | GUI シェル v1.2 の票 5 本 |
| `gui_v13_reviews/` | GUI シェル v1.3 のレビュー記録・クロスリンク・途中保存 |
| `settings/` | 設定レジストリの受入完了票 (S2 / S3 / S3I2 / S4 / S5) |
| `network/` | Host Services の票 (N0〜N4) |
| `kernel_v2/` | カーネル 2.0 の完了記録 8 本 |
| 直下 | 領域に属さない単発の記録 (`REFACTORING_PLAN.md` `ROADMAP_v1.0.md` `TEST_INVENTORY_2026-09-14.md` `debug_kcg_load_font.md`) |

**計画・設計・契約・索引は落とさない。** `PLAN.md` `DESIGN.md` `CONTRACTS.md`
`API_CONTRACTS.md` `TASKS.md` の類は完了後も現行仕様として読まれ続けるので
`docs/tasks/<領域>/` に残る。落とすのは「その作業をやり切った」票だけ。

## 移し方

手で `git mv` しない。**`tools/move_docs.py`** が `git mv` と参照の書き換えを 1 手でやる。

```bash
python3 tools/move_docs.py --into docs/archive/<領域> docs/tasks/<領域>/TASK_X.md ... --dry-run
python3 tools/move_docs.py --into docs/archive/<領域> docs/tasks/<領域>/TASK_X.md ...
```

リポジトリ中の `.md` を舐めて、動いた文書を指す相対リンクと、地の文の
ルート相対パス言及 (票と `tools/tests/*_tdd.md` が互いを指す書き方) を追従させる。
動いた文書自身の中のリンクは、深さが変わるので全部引き直す。
`--dry-run` で書き換え一覧だけ出せるので、**先に見てから当てる**。

移したあとに回すもの (3 つとも `make check` の列に入っている):

```bash
python3 tools/check_docs_links.py              # リンクと見出しアンカー
python3 tools/check_docs_orphans.py            # 索引から辿れなくなっていないか
python3 tools/gen_tests_inventory.py --write   # TESTS.md の「票」列を引き直す
```

`docs/INDEX.md` の索引行と、上の籠の表は人が書く。

## 移したあとも守ること

* **`check-docs-links` が通ること** — 相対パスが 1 段ずれるのはいちばん起きやすい
  壊し方なので、`docs/archive/` はリンク検査から除外していない
  (`tools/check_docs_links.py` の docstring)。
* **索引には残すこと** — 票の行を `docs/INDEX.md` から消さない。リンク先が
  `tasks/…` から `archive/…` に変わるだけ。`check-docs-orphans` は
  `docs/INDEX.md` を唯一の起点に辿るので、消すとそのまま孤児になる。
* **本文は書き換えないこと** — 移動でいじってよいのはリンクだけ。完了記録を
  あとから直すと、当時の判断の記録でなくなる。続きが要るなら新しい票を立てる。
* **票は票のまま** — `tools/tests/*_tdd.md` が根拠として指す票がここへ来ても
  参照は生きる (`check_docs_orphans.py` の起点集合は `docs/archive/**` を含む)。
