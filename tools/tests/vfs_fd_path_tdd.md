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

## 実装レビュー ラリー 1 の修正 (2026-09-24)

Codex (B1〜B6) と Opus (nb1・nb2) の Request changes。試験を先に足し、修正前の
ソースで落ちることを確かめてから直した (RED は該当ファイルだけを HEAD 47435ae に
戻して回した)。

| 項目 | 試験 | RED (修正前) |
|---|---|---|
| cdinst (6) | 段 `cdinst`: 実物の `install_packages` に NORMAL の PATH TOO LONG / APPEND の IO / MINIMAL の失敗 / 全部成功の 4 組 | 修正前は `main` の中に直書きで戻り値を捨てていた (関数が無いので組めない)。変異 3 本 (NORMAL / APPEND / MINIMAL で止まらない) で代える |
| SQLite の失効 (B2) | `ime` 段 `sqlite_stale_legacy` / `sqlite_stale_group`: 旧来と group の両経路で truncate / sync / size / lock / unlock / CheckReservedLock / FileControl / read / write が IOERR、close は通り FD が空く | `FAIL expect_methods_fail: m->xTruncate(pf, 0) == SQLITE_IOERR_TRUNCATE` |
| 失効後の BUSY (B3) | 段 `busy` (ext2、group と旧来の経路) と `ime` 段 `sqlite_stale_group` | `busy`: rename が 0 / NOTFOUND で通る。`ime`: `vfs_fd_rename_busy(... "/db/m.db" ...) == 1` で落ちる |
| FAT の名前比較 (B4) | 段 `nocase` (パスで動き ASCII で畳む種別を 2 つ目にマウント): `CDB` / `X.DB` / `X.DB-JOURNAL` の rename、`DISK.IMG` の unlink が BUSY。`fatfold`: 表が ff.c と一致 | BUSY のはずが NOTFOUND (-2)、`x.db-Journal` への rename は 0 (実際に置いた) |
| hot journal の順序 (B1) | `ime` 段 `hot_journal_before_sql`: 失効 + journal を**検索の前に**置く → 辞書無し、journal は `JJJJ` のまま、旧接続で検索していない (0hit の行が出ない) | 3 ファイル (ime_dict.c / os32_sqlite_vfs.c / vfs_fd.c) を戻すと `first_kanji(...) == ""` で落ちる (= 開き直してしまう、Codex の反例どおり)。ime_dict.c だけ戻すと B2 の防御で journal は残るが `n_0hit == 0` で落ちる |
| IME の管理 API (B5) | `ime` 段 `admin_*`: 失効で clear / list / delete / export が開き直す、失効 + journal で辞書無し、失効を伴わない I/O 1 回で 1 回だけやり直す、export の途中の読み失敗は -4 | ime_dict.c を戻して (4a) を抜いた写しで `FAIL admin_apis: ime_user_clear(d) == 0` |
| 長い DB 名 (nb2) | `kapi_db_v50` の `path_len`: 248 バイトの名前は `kapi_db_open` も `sqlite3_open_v2` も CANTOPEN、247 は開けて書ける。`ime` 段 `journal_name_len` も同じ | `FAIL path_len: kapi_db_open(longp) == -1` (kapi だけ直すと `sqlite3_open_v2(...) == SQLITE_CANTOPEN` で落ちる) |
| hsync の BUSY 表示 (nb1) | `hsync_h2` の (5): rename が BUSY → `replace_failed` + `(BUSY)` + 「使用中で置き換えられなかった」、旧宛先不変、一時ファイルを片づける | 文言が無い |

変異 (`--mutants`): fs / rt / cdinst 35 本、syscalls.c 4 本、ime_dict.c 11 本、
os32_sqlite_vfs.c / vfs_fd.c (ime 段で見る) 7 本。全部落ちる。

```
MUTANTS all 35 killed    (+ 失効した SQLite FD を BUSY から外す、名前比較が FS の規則を見ない、
                            cdinst が NORMAL / APPEND / MINIMAL の失敗で止まらない)
ERRNO MUTANTS all 4 killed
IME MUTANTS all 11 killed (+ 旧接続に SQL を流す前に失効を見ない、export が step の異常終了を EOF に、
                            clear / 学習が旧接続で先に SQL を流す、list / export / delete / clear が
                            I/O エラーで開き直さない)
SQLITE MUTANTS all 7 killed (入出力が失効を見ない、truncate / size / CheckReservedLock が失効を見ない、
                            close も失効で断る、journal 名の長さを見ない、失効 FD を BUSY から外す)
```

検証していないこと (追加): 実機・NP21/W の FAT 媒体での BUSY (ホストでは ext2 の口を
「ASCII で畳む種別」として見ただけ。FAT の表は ff.c と文字列で突き合わせた)。
HostDrv の非 ASCII 名の畳み方 (ホスト側の変換に依る) は区別したまま。

## 実装レビュー ラリー 2 (2026-09-24): FatFs / Win32 が意味を変える名前

FatFs (非 LFN) は `\` を区切り、0x20 以下で名前を打ち切り、`DISK.` を `DISK` と読む。
Win32 は `\` を区切り、末尾の空白と `.` を落とす。BUSY / pinned は 1 バイトの fold で
比べるので、`vfs_rm("/fd0/disk.img ")` が使用中の実体を消せた。規則は
`fs/vfs_name_rules.inc` (FAT は途中の空白も断る、Win32 は末尾だけ) で、
`fs/fatfs_vfs.c` は `ff_make_path` (全入口が通る)、`fs/hostdrvfs.c` は `hostdrv_create`
と `hdrv_rename` の宛先で INVAL を返す。

| 項目 | 試験 | RED (bf0dc31 の fs/ + 規則ファイルだけ置いた写し) |
|---|---|---|
| FAT の入口 | 段 `fatname`: **実物の `fs/fatfs_vfs.c` + `fs/fatfs/ff.c`** を RAM の FAT12 (f_mkfs) で。pinned の `DISK.IMG` / `D/IMG.DAT` / `DISK` と開いている SQLite `DB` に `disk.img ` / `disk.img ignored` / `disk.img\x01` / `\disk.img` / `d\img.dat` / `disk.` / `db.` / `DB ` / `db\t` を rm / rename (両引数) → INVAL、open / stat / read / write / mkdir / rmdir / ls も INVAL、実体と中身は不変。正当な 8.3 名 (`/bin/ls.bin`、`/etc/system.cfg`、`.x`、大文字小文字違い)・根の ls・`settings.db-journal` の stat = NOTFOUND は従来どおり | 32 件落ちる。`fat_slurp("0:/DISK.IMG") == 9` 等 = **pinned の実体が消えた・動いた** |
| HostDrv の規則 | 段 `hostname` (合成ドライバ: ext2 の口の前に Win32 の規則): 末尾の空白・`.`・`\`・制御文字が INVAL、`my file.txt` は通る | 合成ドライバなので RED は無い。配線は `hostwire` (本文の検査) と変異で見る |
| 規則の表 | 段 `namerule`: 21 行 × 2 規則 | — |
| hostdrvfs.c の配線 | `hostwire`: 名前をホストへ渡す口は setup_create → session_set_path だけ、hostdrv_create がその前に断る、rename の宛先は元を開く前、パスを受ける 10 口は全部 hostdrv_create(path) を通る | 検査を外した本文 3 通りで FAIL を確認 |
| ime の list (非 blocker) | `ime` 段 `admin_list_prepare_error`: dict_user を DROP → list は -4 (0 件でない)、開き直さない | 変異「list の prepare の失敗を 0 件にする」 |

変異: fs 側 +8 本 (FAT の入口が名前を見ない、`\` / 制御文字 / FAT の途中の空白 /
末尾の空白 / 末尾の `.` を通す、Win32 でも途中の空白を断る、`.` / `..` の要素まで断る)
で計 43 本、ime_dict.c +1 本で 12 本。全部落ちる。

検証していないこと: 実機・NP21/W の FAT 媒体と HostDrv (Windows) の上での動作。
Win32 の他の別名 (8.3 の短い名前 `DISK~1.IMG`、`:` の代替データストリーム、
`CON` などの予約名) は規則に入れていない。
