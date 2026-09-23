# TASK_VFS_FD_PATH — FD がパスを覚えて書き込みごとに引き直す / 長いパスを黙って切り詰める (VFS の既存欠陥)

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-24) / 状態: **v2 — ラリー 1 (Codex / Opus とも Request changes) を反映、ラリー 2 待ち** — ユーザー指示「別票を着手」(2026-09-24)。カーネル層 (VFS/FS) の既知の欠陥なので POLICY_DEV §1 に沿って新機能より先に扱う。
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

## 方針 v2 (ラリー 1 を反映、上の方針 1〜8 を置き換える)

**同一性は ctime / links / generation に頼らない** (両者 A-1、Codex A-2: ext2 の時刻は定数 `0x67E8E800`、generation は常に 0、inode は最小番号から再利用、set_mtime / O_TRUNC で ctime が変わる)。代わりに **VFS 側の失効 (invalidation)**:
1. FD は open 時に (マウント, inode 番号) を記録する (ext2 に `ino_of(ctx, path, &ino)` を追記。FS が持たなければ記録しない)。
2. `vfs_rm` / `vfs_rmdir` / `vfs_rename` (上書きされる宛先と移動元の両方) は、操作の前に対象の inode を取り、**成功したら**同じ (マウント, inode) の開いた FD 全部に `stale` 印を付ける。`vfs_umount` は**そのマウントの FD 全部**に印を付ける (Codex A-3: 解放済みの fs_ctx を触らない — 印は fs_ctx を参照する前に見る)。
3. read / write / fstat / 切り詰め / seek は最初に印を見て、立っていれば `OS32_ERR_STALE` (**既存の -11** を使う) を返す。読み書きのたびの追加 lookup は無い (ext2 の ns_gen パスキャッシュもそのまま)。
4. `ext2_write_stream` / `read_stream` / 切り詰めは通常ファイル以外を `ISDIR` で断る (最後の砦)。
5. 恒久策 (inode を保持し、最後の FD が閉じるまで unlink した実体を解放しない = POSIX) は別票。今回の保証は「ext2 で、名前空間の変更後に古い FD が別の実体へ書かない」。FAT / HostDrv は従来どおり (ISO は書き込み対象外)。
6. **常駐 FD の持ち主** (Opus A-2): IME 辞書 (`kernel/ime_dict.c`) は STALE / IO を受けたら閉じて開き直す (1 回、失敗なら辞書無しで動く)。loop_dev と cfg_backend の長く開いた接続は同じ扱いを入れるか、置き換え時に閉じる — 実装で全持ち主を列挙して報告。
7. **newlib の境界** (Codex A-4): `sdk/crt/syscalls.c` の `_write` / `_read` / `_open` / `_close` / `_lseek` / `_fstat` は OS32 の負値を **-1 + errno** に変換する (STALE → ESTALE、NAMETOOLONG → ENAMETOOLONG、ISDIR → EISDIR、ほか EIO)。SDK の変更なので `make external` の対象。

**長いパス** (両者):
8. `vfs_resolve_path` は**コピーの前に**長さを検査し (NUL 抜き 255 バイトまで)、要素の積み上げで表 (`VFS_MAX_PATH_DEPTH` 32) が満杯になった**時点で**断る (正規化後の数ではない)。戻り値を int にし、全呼び手 (vfs.c 14 か所、vfs_fd.c、kapi_db.c、ホスト試験の写し) を直す。`exec/exec.c:1430` の `"/usr/bin/"+path` の切り詰めも範囲に入れる。`OS32_ERR_NAMETOOLONG` は **-16** に新設し、ネットワークの予約 (-16 以降) を 1 つ進めて `os32_kapi_shared.h` と KAPI_SPEC §3-2 を更新。
9. **パッケージ**: mkpkg は UTF-8 バイト長で `/hd0` 前置込み NUL 抜き 127 以下 (格納パス 123 以下)、項目数 ≤ `PKG_MAX_ENTRIES` (128) を検査し、255 での黙った切り詰めを廃止。`pkg_parse` は過長項目・項目数超過で**即エラー** (読み飛ばさない)。`pkg_extract` は sys_open / sys_write の失敗を返し、cdinst は失敗を表示して止める。
10. **末尾の `.` / `..`**: 末尾の `/` を落とした後の最終要素が `.` / `..` なら、rmdir / rename (両引数) / unlink / mkdir を正規化前に INVAL。
11. **マウント点の O_EXCL**: NOSYS の判定の後に EXIST。
12. 受入 (追加): 通常ファイルへの inode 再利用 (open → unlink → 同名 create → 旧 FD write が STALE、新ファイル不変)、ディレクトリへの作り直し、set_mtime / O_TRUNC 後も同じ FD は STALE にならない、umount 後の旧 FD、read / fstat、相対パス + cwd、255/256 と 32/33 の境界、mkpkg 123/124 と 128/129 項目、newlib の write が -1 + errno、IME 辞書の置き換え後に FEP が動く (NP21/W)。
