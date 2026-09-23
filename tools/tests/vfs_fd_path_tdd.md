# vfs_fd_path TDD 記録 — FD は inode で動いて失効する / 長いパスは切り詰めずに断る

対象票: [`docs/tasks/memory/TASK_VFS_FD_PATH.md`](../../docs/tasks/memory/TASK_VFS_FD_PATH.md) (方針 v2 の 8〜12、v3、ラリー 3 とユーザー決裁 ①)
試験: `tools/tests/vfs_fd_path_host.c` (実物の `fs/ext2_*.c` + `fs/vfs.c` + `fs/vfs_fd.c` + `userland/lib/rt/pkg.c` を `#include`)、
`tools/tests/newlib_errno_host.c` (実物の `sdk/crt/syscalls.c` を newlib のヘッダで)、
`tools/tests/ime_dict_host.c` (実物の `kernel/ime_dict.c` + SQLite + `lib/sqlite3/os32_sqlite_vfs.c` + `fs/vfs_fd.c`)
実行: `python3 -B tools/tests/test_vfs_fd_path.py [--target] [--mutants] [--red[=REV]] [case]` / `make check-vfs-fd-path-host`

基点: `wt/vfs-fd` = `6de1fea`。

## 何を見ているか

| 段 | 反例 / 境界 | 期待 |
|---|---|---|
| fd | 欠陥 1: `open(f, O_CREAT)` → `unlink f` → `mkdir f` → 旧 FD へ write | **STALE**、ディレクトリのブロック不変、書き込み 0 セクタ |
| fd | 通常ファイルへの inode 再利用 (unlink → 同名 create、同じ inode 番号) | 旧 FD の read / write / fstat / seek が STALE、新ファイル不変 |
| fd | `set_mtime`・別 FD の `O_TRUNC` | 同じ FD は失効しない |
| fd | 親ディレクトリの rename の後 | 旧 FD は同じ実体を読み書き (新しい同名ファイルは不変) |
| fd | 置き換え rename / `rename(f,f)` / 公開後の失敗 (注入) / 何もせず失敗 (注入) / 宛先なし | 宛先の旧 FD だけ STALE (成否を問わず)、元・自分自身は失効しない |
| fd | ハードリンク (ホストで作った形) | 名前間の rename は失効させない、片方の unlink で同じ inode の FD は STALE (余分に断る側) |
| fd | inode 取得の失敗注入 | open は失敗 (O_CREAT / O_EXCL で作ったものは消す)、unlink / rename は操作しない |
| fd | `ext2_read_stream` / `ext2_write_stream` / inode の切り詰め・書き込みにディレクトリ | ISDIR (書かない) |
| fd | マウント点の `O_EXCL` | EXIST |
| fd | umount | 全 FD が STALE、fs_ctx を外す、close は通る |
| busy | 開いている SQLite DB / ジャーナル / 祖先ディレクトリの rename (元・宛先) | **BUSY**、書かない、印も付けない。似た名前・兄弟は通る。close 後は通る。旧来の vfs_open 経路も印で同じ |
| busy | 使用中の loop イメージの unlink / 置き換え | BUSY。移動は通り、FD は同じ実体を読む |
| dot | 最終要素の `.` / `..` (末尾 `/` 付きも) の rmdir / rename 両引数 / unlink / mkdir | **INVAL**、書かない (`rmdir a/.` が a を消さない) |
| path | NUL 抜き 255 / 256 バイト (`…X` と `…Y`)、出力の器が小さい、入力 262 バイト (畳むと短い) | 255 は通る、256 以上は **NAMETOOLONG** (書かない) |
| path | 32 / 33 要素、33 個目の後の `..`、途中で `..` が戻る形 | 32 は通る、33 は NAMETOOLONG (旧実装の mkdir -p は EXIST) |
| path | 相対名 + 206 バイトの cwd | 結果で判定 (連結が 256 を超えても結果 65 バイトなら通る、結果 256 は断る) |
| path | 256 バイトの mount prefix | NAMETOOLONG、登録しない |
| pkg | `pkg_parse` 127 / 128 バイト、128 / 129 項目 (ヘッダは 128 と偽る)、終端の無い表、項目数の食い違い | 127・128 項目は通る、他は即エラー |
| pkg | cdinst の前置 (`pkg_first_overflow`) 格納 123〜127 バイト | 123 は展開できる、124〜127 は展開の前に断る (0 セクタ) |
| pkg | 置き場がディレクトリ (無圧縮 / LZSS) | `pkg_extract` が PKG_ERR_IO (旧実装は OK) |
| mkpkg | 実物の `tools/mkpkg.py`: 123 / 124 バイト、UTF-8 123 / 126 バイト、300 バイト、128 / 129 項目 | 上限を超えたら終了コード 1 (255 の切り詰めは無い) |
| errno | `_read` `_write` `_open` `_lseek` `_fstat` `_stat` `_unlink` に STALE / NAMETOOLONG / ISDIR / BUSY / NOTFOUND / EXIST / NOSPC / IO / ROFS / 未知 | -1 + ESTALE / ENAMETOOLONG / EISDIR / EBUSY / ENOENT / EEXIST / ENOSPC / EIO / EROFS / EIO。非負はそのまま、`_close` は int 0 |
| ime | 辞書の実体の差し替え + FD の失効 | SQLite 接続ごと開き直して新しい中身で答える (失効 1 回につき 1 回)、protect / SQLite の印も付け直す |
| ime | 失効を伴わない I/O エラー | 1 回だけ開き直す (成功で戻す)。読めないままなら繰り返さない。開けなければ辞書無し |
| ime | 開き直しの時点で `<辞書>-journal` が中身を持って残っている / 長さ 0 | 開かない (辞書無し、画面に出す、journal に触らない) / 開き直す |
| ime | 学習 (UPSERT) の失効 | 開き直して 1 回やり直す |

fd / busy / dot / path / pkg の像は本物の `e2fsck -fn` に当てる (**終了コード 0 以外は失敗**)。

## RED (`--red`、`6de1fea` の fs/ + userland/lib/rt/ + sdk/crt/syscalls.c)

失効の印・inode の口を直接見る検査と注入は RED では組まない (`-DFDP_RED`)。

```
case fd
  FAIL line 451: rc_ == (-11)            ← 欠陥 1: 旧 FD の write が 24 を返した
  FAIL line 451: wr_non_state() == w0_
    ^ rc=24
  FAIL line 455: i == EXT2_BLOCK_SIZE    ← 作り直したディレクトリのブロックが書き換わった
  FAIL line 472: slurp("/g", …) == 3 && streq(buf, "NEW")   ← inode 再利用で新ファイルを上書き
  FAIL line 500/501: 親の rename 後の旧 FD が新しい同名ファイル /d/f へ書いた
  e2fsck -fn fd.img: rc=12 FAIL
    | Directory inode 11, block #0, offset 0: directory corrupted
case busy   BUSY の代わりに rename が通る (DB・ジャーナル・祖先・宛先)、loop イメージが消える
case dot    rmdir("/hd0/a/b/.") が 0 (b を消す)、mkdir("…/.") が EXIST、rename が通る
case path   256 バイトが通る (…X と …Y が同じ名前)、33 要素の mkdir が 0、入力 262 バイトが通る
case pkg    128 バイトのパスで 0、129 項目で 0、切れた表で 0、前置で溢れる 124〜127 を展開 (書き込みあり)、
            置き場がディレクトリでも PKG_OK
checks 332 failures 146
RED reproduced
case errno  全関数が負値をそのまま返す (errno 立たず) — checks 78 failures 70
RED errno reproduced
```

ime の RED は組まない (修正前の `IME_Dict` に `io_retried` が無い)。変異 (下) で代える。

## GREEN

```
case errno (real sdk/crt/syscalls.c + newlib headers)   checks 78 failures 0
case mkpkg (real tools/mkpkg.py)                         7 件 ok
case ime (real kernel/ime_dict.c + SQLite + vfs_fd.c)    SUMMARY ime_dict PASS (7 段)
case fd     e2fsck -fn fd.img: clean
case busy   e2fsck -fn busy.img: clean
case dot    e2fsck -fn dot.img: clean
case path   e2fsck -fn path.img: clean
case pkg    e2fsck -fn pkg.img: clean
checks 367 failures 0
TARGET i386-elf -Werror compile PASS (fs/vfs.c, fs/vfs_fd.c, fs/ext2_vfs.c, fs/ext2_file.c, fs/fatfs_vfs.c, fs/hostdrvfs.c, fs/iso9660.c)
```

## 変異 (`--mutants`、写しを変異させるので実物は書き換えない)

fs / rt 30 本、syscalls.c 4 本、ime_dict.c 3 本。全部落ちる。

```
MUTANTS all 30 killed          (失効の印を見ない ×4、パスで書く、unlink / rename / umount が印を付けない、
                                成功時だけ印、rename(f,f) に印、取得失敗で消す、作ったものを消さない、
                                SQLite / ジャーナル / loop の BUSY、ext2 の ISDIR ×2、O_EXCL の EXIST、
                                長さを先に見ない、要素を捨てる、結果を切り詰める、末尾 . ×2、
                                mount prefix、pkg_parse ×3、前置が 1 バイト甘い、pkg_extract ×2)
ERRNO MUTANTS all 4 killed
IME MUTANTS all 3 killed       (hot journal を見ない、I/O で何度でも開き直す、失効しても開き直さない)
```

## 検証していないこと

- **NP21/W 上で IME 辞書を置き換えた後に FEP が動くこと** (票 v2 の 12 の最後)。ホストの `ime` 段は
  実物の ime_dict.c + SQLite で開き直しの判断を見ているが、ゲストの画面・キー入力は通していない。
- FAT / HostDrv の FD は従来どおりパスで動く (票の範囲外)。
