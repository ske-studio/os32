# TASK_VFS_FD_PATH — FD がパスを覚えて書き込みごとに引き直す / 長いパスを黙って切り詰める (VFS の既存欠陥)

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-24) / 状態: **方針レビュー (Codex + Opus、ラリー 1)** — ユーザー指示「別票を着手」(2026-09-24)。カーネル層 (VFS/FS) の既知の欠陥なので POLICY_DEV §1 に沿って新機能より先に扱う。
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

## 方針 (レビュー対象)

1. **FD の同一性**: `VfsOps` に任意の操作 `ident(ctx, path, u32 *id_hi, u32 *id_lo)` を追記する (ext2 は inode 番号 + inode の生成時刻 (ctime) か、世代が無ければ inode 番号と i_links/i_ctime の組)。`vfs_open` で FD に同一性を記録し、`vfs_read_fd` / `vfs_write_fd` / `vfs_fstat` / 切り詰めのたびに再解決した同一性と比べ、**違えば `OS32_ERR_STALE` (新設) で断る** (書かない)。`ident` を持たない FS (FAT / ISO / HostDrv) は従来どおり。
2. **型の検査を FS 側にも**: `ext2_write_stream` / `ext2_read_stream` / 切り詰めは、解決した inode が通常ファイルでなければ `ISDIR` で断る (1 が無い経路・将来の FS の最後の砦)。
3. **長いパスは切り詰めずに断る**: `vfs_resolve_path` は、正規化の前後どちらかで `OS32_MAX_PATH` (256、終端込み) を超える、または要素数が `VFS_MAX_PATH_DEPTH` (32) を超えるとき **`OS32_ERR_NAMETOOLONG` (新設)** を返し、呼び手 (VFS の全入口) はそれを返す。要素 1 つが 255 バイトを超えるのも同じ。
4. **パッケージ**: `tools/mkpkg.py` は UTF-8 **バイト長**で、展開先の前置 (`/hd0`) を含めて `PKG_MAX_PATH` (128) 未満を検査して断る。`userland/lib/rt/pkg.c` の `pkg_parse` はパスが上限以上なら**読み位置をずらさずに**エラーを返す (項目を読み飛ばす長さはヘッダの値で進める)。cdinst の前置は溢れたら断る。
5. **マウント点への `O_CREAT|O_EXCL`** は `EXIST` (vfs.h:131 の契約どおり)。
6. **末尾の `.` / `..`** (`rmdir("/a/.")` 等): POSIX と同じく、rmdir / rename / unlink の**最終要素が `.` または `..`** なら正規化の前に `INVAL` で断る。mkdir の `mkdir a/.` も INVAL (EXIST にしない)。
7. エラーコードの新設 (STALE / NAMETOOLONG) は `sdk/include/os32/os32_kapi_shared.h` の `OS32_ERR_*` に**追記** (KAPI のスロットは増えない)。シェル・hsync の表示を足す。
8. 受入: ホスト試験 (実物の vfs + ext2) で、欠陥 1 の反例 (open → unlink → mkdir → write が STALE で断られ、ディレクトリのブロックが変わらない、e2fsck -fn clean)、256/257 バイトと 32/33 要素の境界、mkpkg / pkg_parse の 127/128 バイト境界、O_EXCL の EXIST、`rmdir a/.` の INVAL。
