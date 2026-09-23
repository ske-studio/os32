# TASK_VFS_FD_PATH — FD がパスを覚えて書き込みごとに引き直す / 長いパスを黙って切り詰める (VFS の既存欠陥)

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-24) / 状態: **起票 (未着手)**。カーネル層 (VFS/FS) の既知の欠陥なので POLICY_DEV §1 に沿って新機能より先に扱う。
> 出所: TASK_EXT2_EMPTY_NAME の修正 (fc5ce67) の実装レビュー (Codex / Opus とも Approve、どちらも「修正前からある別の欠陥」として挙げた)。

## 欠陥 1 (Codex、優先): 開いた FD の書き込みが、同じパスに作り直したディレクトリを上書きする

- 反例: `open("/hd0/f", O_CREAT|O_WRONLY)` → `unlink("/hd0/f")` → `mkdir("/hd0/f")` → 元の FD へ `write`。FD は inode ではなくパスを保持し、書き込みのたびに再解決する (`fs/vfs_fd.c:381` `vfs_write_fd`)。その先の `ext2_write_stream` (`fs/ext2_file.c:311`) に通常ファイルの型検査が無く、新しいディレクトリのブロックを任意データで上書きできる (空名項目も作れる)。
- 直し方の候補: (a) FD が inode 番号 (と世代) を持ち、書き込みは inode で行う。(b) 最低限、write_stream で通常ファイルでなければ ISDIR で断る + open 時の inode と再解決した inode が違えば IO/STALE で断る。

## 欠陥 2 (両者): 長いパス・深いパスを拒否せず切り詰める

- `vfs_resolve_path` (`fs/vfs.c:56`) は 256 バイトの一時領域へコピーし、超えた分を捨てる。`/` + `a`×254 + `X` と `…Y` が同じ名前に解決される → 書き込み・削除・rename が別の対象へ作用し得る。要素数が `VFS_MAX_PATH_DEPTH` (32) を超えると超えた分を捨てる (深い `mkdir -p` が EXIST で成功に見える)。
- `tools/mkpkg.py` はパスの形だけ検査し、消費側の `PKG_MAX_PATH` = 128 を見ない。`/hd0` 前置後の切り詰めで後のファイルが前を上書き、128 以上ではパーサの読み位置がずれる (`userland/lib/rt/pkg.c:164`、`userland/system/cdinst.c:284`)。
- 直し方: どれも**切り詰めずに断る** (NAMETOOLONG 相当)。mkpkg は UTF-8 バイト長で展開先の前置を含めて検査。

## そのほか (記録)

- マウント点への `open(O_CREAT|O_EXCL)` は EXIST でなく INVAL (契約 vfs.h:131 と不一致、到達する呼び手は無い)。
- `rmdir("/hd0/a/.")` は正規化で `a` 自体を消す (正規化前の検査が要るかは仕様判断)。
