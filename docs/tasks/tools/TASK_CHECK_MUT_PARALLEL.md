# TASK_CHECK_MUT_PARALLEL — `make check` の変異段 (check-mut) を並列にする

> 状態: **票 (未着手)**。発行: PM (Claude Code `claude-opus-5-5`)、2026-09-26。v2.1 の後。
> 関係: [`docs/POLICY_DEBUG.md`](../../POLICY_DEBUG.md) §4-40 (打ち切りで変異が残る)・§4-41 (2 段に割った経緯)、`build/sdk.mk` の `check:` / `check-mut:`。

## 0. 背景

16 コアの機械で `make check` の後半がほぼ 1 コアで回っている (ユーザーの指摘、2026-09-26)。
`check:` は 2 段で、1 段目 `check-par` は並列、2 段目 `check-mut` は `-j1`。2 段目の 14 本は
**実物のソースに変異を当てて戻す**作りなので、同じ作業ツリーで並べると互いの変異がぶつかる。
いまの `make check` は 1 回 5〜10 分以上で、そのほとんどがこの段。

すでに多くの試験は**一時ディレクトリの写しに変異を当てる**作りへ移してあり、`check-par` で回っている
(`build/sdk.mk` の「写しの上で変異させるので並列 (check-par) で回せる」の注記、例: `test_hsync_h1.py`)。
残りの 14 本を同じ作りに変えれば、`check-mut` の段そのものが要らなくなる。

## 1. 対象 (2026-09-26 の `check-mut`)

`check-kapi-layout-host` `check-edit-doc-host` `check-fstat-redir-host` `check-kstring-c-host`
`check-kstr-bench-host` `check-sh-status-host` `check-hsync-h3-host` `check-hsync-h2-host`
`check-h4-manifest-host` `check-vfs-excl-host` `check-fs-kind-callers-host` `check-cat-linenum-host`
`check-result-conv-host` `check-guest-host`

## 2. やること

1. **まず測る**: 14 本それぞれの所要時間を測り、長い順に並べる (`/usr/bin/time` か各スクリプトの経過時間)。
   効果の大きいものから移す。
2. 各試験を「写しに変異を当てる」作りへ: 変異が触るファイルと、ビルドに要る最小の木 (ヘッダ・ハーネス) を
   `tempfile.TemporaryDirectory` に写し、その中で変異・ビルド・実行する。**実物のソースは読むだけ**。
   写しの作り方は既存の移行済みの試験に合わせ、共通部品があれば使う (無ければ `tools/tests/` に 1 つ作る)。
3. 移したものは `check-mut` から `check-par` へ。全部移ったら `check-mut` の段と `-j1` を消す
   (`check_tree_unchanged.py` の番人は残す — 写しの作りが崩れて実物を書いたら 1 段目で捕まる)。
4. `check-par` 自身の並列度が効いているか (`make -j` が下位の make に渡っているか) も確かめる。

## 3. 受け入れ

- C1: `make check` の所要時間を移行前後で測って票に残す (同じ機械、同じ木)。
- C2: 変異の結果 (RED の本数、SURVIVED 0) が移行前と同じ。
- C3: `make check` を 2 つの worktree で同時に流しても、どちらも通り、どちらの木にも変異が残らない。
- C4: 途中で打ち切っても実物のソースに変異が残らない (§4-40 の罠が構造上なくなる)。

## 4. しないこと

試験の中身 (何を確かめるか) は変えない。移すのは変異を当てる場所だけ。
