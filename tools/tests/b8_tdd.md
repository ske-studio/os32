# 票 B8 — 読み取り失敗を「不存在」として扱う処理の是正 (ホスト TDD の記録)

- 票: [`docs/tasks/shell/TASK_FS_TYPE.md`](../../docs/tasks/shell/TASK_FS_TYPE.md) §2
- 基点: `feat/gui` の `3316fdb`
- 実行: `python3 -B tools/tests/test_b8_open.py --target`
  (`make check-b8-open-host` が同じものを回す)
- 日付: 2026-09-15

## 0. 正直に書く ([V4])

**試験は実装のあとに書いた。** `fs/ext2_inode.c` / `ext2_dir.c` / `ext2_file.c` /
`ext2_vfs.c` / `vfs.c` / `vfs_fd.c` / `hostdrvfs.c` / `hostdrv_stat_rules.inc` を
先に直し、そのあとで `tools/tests/b8_open_host.c` を書いた。
だから「先に RED を見た」とは書けない。

代わりに **変異試験で赤を取った**。直した規則を 1 つずつ元に戻した版に差し替えて
試験を回し、**18 件すべてが落ちる**ことを確かめてある (§4)。落ちなければ
「試験がその規則を見ていない」ということなので、そこは数えて正直に書く。

実際、最初に書いた版では **M9 / M10 / M18 の 3 件が GREEN のまま通った**
(= 取り逃していた)。その 3 つを捕まえるために試験を足し、締め直した経緯も §4 に残す。

## 1. 何を確かめる試験か

`vfs_path_kind()` の戻り値までしか見ない試験では、同じ形の欠陥 (B7) を
Codex 実装レビュー 往復 4 で**実際に取り逃している**。だから
**判定を消費する側 = 実物の `vfs_open()` / `vfs_open_sqlite()`** まで通す。

`tools/tests/b8_open_host.c` は 1 本の実行ファイルに 3 つの段を載せる。

| 段 | 土台 | 何を見るか |
|---|---|---|
| A | **実物の ext2** (`fs/ext2_super.c` / `ext2_inode.c` / `ext2_dir.c` / `ext2_file.c` / `ext2_fmt.c` / `ext2_vfs.c`) + **実物の `fs/vfs.c` / `fs/vfs_fd.c`**。贋物は Device API と IDE だけ。RAM 上の 8MB を実物の `ext2_format()` で作る | **間接ブロックを使う大きなディレクトリ**の読み出しを**一度だけ**落とし、`ext2_bmap` → `find_entry` → `lookup` → `stat` → `vfs_open` の全段で「未割当 = 検索終了」に化けないこと |
| B | 合成 `VfsOps` (`tools/tests/vfs_kind_host.c` と同じ作法) | `stat` と `get_file_size` の戻り値を 1 つずつ指定し、**`write_file` の呼び出し回数**で「作成へ進んでいない」ことを 0 件で押さえる |
| C | `fs/hostdrv_stat_rules.inc` の純関数 | HostDrv の `get_file_size` がディレクトリに答えないこと (`hdrv_size_result`)。ハイパーコールを叩く `fs/hostdrvfs.c` 本体はホストで組めないので、判定だけを純関数へ切り出した (票 H1 の `hdrv_stat_fill` と同じ作法) |

**一度だけ**の失敗にするのが肝。恒久的な故障なら誰が見ても異常だが、B8 が起きるのは
「一度読めなかっただけで次は読める」ときである。注入器は
「この LBA の n 回目の読み出しを落とす」(`fail_arm`) と
「armed のあいだずっと落とす」(`fail_arm_always`) の 2 つを持つ。
**仕掛けが本当に効いたか**を毎回 `g_fail_fired` で確かめているので、
「狙った経路に届いていないのに緑」にはならない。

### 段 A の足場

- `/big` に 6 文字名の詰め物を 900 件入れる。1 エントリ 16 バイト = 1 ブロック 64 件
  なので、直接ブロック 12 本 (768 件) を確実に越えて**間接ブロックを使う**。
  `lba_of_big_indirect()` が `inode.block[EXT2_IND_BLOCK] != 0` を CHECK するので、
  詰め物が足りなくなればその場で落ちる。
- 目当ての `/big/keepme` は**最後**に入れる = 間接ブロック側に載る。
- 中身が残っているかは **`vfs_open` を通さず** `ext2_read_file` で読み直す
  (`keep_intact()`)。open が壊したかどうかを open 自身に聞かない。

## 2. 追加した試験と件数

`python3 -B tools/tests/test_b8_open.py` → **261 checks, 0 failures**。

| 段 | 名前 | 中身 |
|---|---|---|
| A5 | `case_bmap_contract` | `ext2_bmap` の約束 — 未割当は `EXT2_OK` + `0`、読めなければ `EXT2_ERR_IO`。同じ経路が `find_entry` / `lookup` / `ext2_vfs_stat` / `vfs_path_kind` / `ext2_vfs_get_size` まで畳まれずに届くこと |
| A1 ×4 | `case_indirect_read_failure` | 間接ブロックの一度の失敗 × (`O_CREAT` 有無 × `O_TRUNC` 有無)。FD が出ない / `NOTFOUND` と言わない / **媒体に 1 セクタも書かない** / 中身が残る |
| A2 ×4 | `case_size_failure` | 種別は `stat` で確定しているのに**サイズ取得だけ**が一度失敗する (§2-2b、**最も重い無言のデータ消失**) × 4 通り |
| A3 | `case_sqlite_size_failure` | `vfs_open_sqlite` も同じ |
| A6 | `case_write_file_failure` | 一括書き込みの既存判定が読めないとき、**新規作成へ落ちない**。一度きりの失敗と恒久的な失敗の両方 |
| A7 | `case_write_stream_failure` | 追記書き込みが、読めない間接ブロックを「未割当」と見なして**割り当て直さない** (元のブロックの 1024 バイトが丸ごと残ることまで確認)。正常時に同じ書き込みが通る回帰つき |
| A8 | `case_other_bmap_callers` | `ext2_bmap` の**残りの呼び手** 6 系統 — `list_dir` (打ち切りを成功にしない) / `add_entry` (新ブロックを継ぎ足さない) / `delete_entry` / `read_file` `read_stream` (短いファイルに化けない) / `is_dir_empty` (`NOTEMPTY` と偽らない) / `parent_of` (rename の循環検査、**木が輪にならない**) |
| A4 | `case_normal_paths` | 正常系の回帰 — 通常ファイルは開ける / ディレクトリは `ISDIR` / 本当に不存在なら `O_CREAT` で作れる / `O_TRUNC` は切り詰まる / `O_CREAT` 有でも既存の中身は残る |
| B1 ×7 | `synth_size_failure` | 4 通り + `NOSPC` / `NOTDIR` / `INVAL`。**`write_file` の呼び出し回数が 0**、中身の長さが変わらない |
| B2 B3 | `synth_trunc_write_failure` | `O_TRUNC` / `O_CREAT` の `write_file` が失敗したら open しない |
| B4 | `synth_sqlite` | `vfs_open_sqlite` の作成抑止 + 本当に不存在なら作れる回帰 |
| B5 | `synth_normal` | 合成ドライバでの正常系 + `stat` を持たないドライバのプローブが `ISDIR` を `DIR` と読むこと |
| C1 | `stage_c` | `hdrv_size_result` — ディレクトリ / 問い合わせ失敗。**問い合わせは 1 回のまま** (種別は同じ応答の `Directory` で分かる。NP21/W は `FileStandardInformation` にこれを埋める) |

## 3. 既存の試験を 1 行直した

`tools/tests/ext2_write_io_host.c` の `ns_invalidation` が

```c
CHECK(ext2_vfs_get_size(g_ec, "/tmp/d", &sz) == VFS_OK);   /* /tmp/d はディレクトリ */
```

と書いていた。これは「記憶が名前空間に追従するか」を見るだけの行で、
**当時の `ext2_get_size_ino` がディレクトリの inode サイズをそのまま返していた挙動を
そのまま写していた**。その成功が open の受け手で「通常ファイルである」根拠に使われ、
`cat <ディレクトリ>` を通していた ③ の本体なので、期待値を `VFS_ERR_ISDIR` に直した。
理由はその場にコメントで残してある。

## 4. 変異試験 (18 件すべて RED)

直した規則を 1 つずつ元に戻して `test_b8_open.py` を回す。

| # | 戻した規則 | 落ちた件数 |
|---|---|---|
| M1 | `ext2_bmap` が I/O エラーを未割当 (0) に潰す | 47 |
| M2 | `ext2_lookup` が `find_entry` のエラーを `NOTFOUND` に畳む | 33 |
| M3 | `ext2_find_entry` が `bmap` のエラーを無視して検索終了にする | 35 |
| M4 | `ext2_vfs_*` が `resolve_path` のエラーを一律 `NOTFOUND` にする | 32 |
| **M5** | **② の肝**: open が `NOTFOUND` 限定を外して作成へ進む | **54** |
| M6 | `O_TRUNC` が `write_file` の戻り値を無視する | 2 |
| M7 | `ext2_get_size_ino` がディレクトリを断らない | 1 |
| M8 | `hdrv_size_result` がディレクトリを断らない | 3 |
| M9 | `ext2_vfs_write` が読めないときも新規作成へ落ちる | 1 |
| M10 | `ext2_write_stream` が読めないブロックを割り当て直す | 4 |
| M11 | `vfs_path_kind` のプローブが `ISDIR` を「無い」に潰す | 1 |
| M12 | `ext2_list_dir` が打ち切りを成功にする | 2 |
| M13 | `ext2_add_entry` が読めないまま新ブロックを継ぎ足す | 2 |
| M14 | `ext2_delete_entry` が `NOTFOUND` に化けさせる | 1 |
| M15 | `ext2_read_file` が短いファイルにする | 1 |
| M16 | `ext2_read_stream` が短い読み出しにする | 1 |
| M17 | `ext2_is_dir_empty` が読めなかったのを「空でない」にする | 2 |
| M18 | rename の循環検査が読めなかったのを「祖先ではない」にする | 4 |

### 最初に取り逃した 3 件

- **M9** — 一度きりの失敗だと、`ext2_create` の中の存在確認が今度は通って
  `EXT2_ERR_EXIST` になる。`rc < 0` としか見ていなかったので緑だった。
  **`rc == VFS_ERR_IO`** まで見るように締めて赤になった。
  「読めなかったことを読めなかったと言う」が規則そのものなので、これが正しい形。
- **M10** — `ext2_write_stream` を叩く試験がそもそも無かった。16KB の
  `/big/wide` を作って間接領域へ追記する A7 を足した。
- **M18** — `fail_arm_always` で落としていたので、循環検査を素通りしても後段の
  `ext2_add_entry` が止まり、被害 (木が輪になる) が見えなかった。
  d2 の先頭ブロックの**2 回目の読み出しだけ**を落とす形に変えて赤になった。

この 3 件は「① の修正が別の段で被害を先に止めてしまい、変異の影響が見えない」
という形の取り逃しだった。**被害ではなくエラーの名前まで見る**か、
**影響が出る経路を狙って落とす**かのどちらかが要る。

## 5. 触っていないもの

- `sdk/kapi.json` (KAPI は増やしていない。`make clean` からの全ビルドは不要)
- エミュレータ・実配備・`*.ini` — 一切触れていない
- `make` の各ターゲット — 走らせていない。ホスト試験の直接実行と
  `i386-elf-gcc` の単体コンパイルだけ

## 6. 実機で見ていないこと ([V4])

- **`hdrv_get_file_size` のディレクトリ拒否は実機で未確認。** 判定は純関数
  (`hdrv_size_result`) に切り出してホストで試験してあるが、ゲストから実際に
  `Np2FileStandardInfo.Directory` が返ってくるところはエミュレータ上で見ていない。
  根拠は NP21/W 側のソース — `hostdrvNT_IRP_MJ_QUERY_INFORMATION` の
  `FileStandardInformation` 枝が `GetFileAttributesEx` の
  `FILE_ATTRIBUTE_DIRECTORY` から `stdInfo.Directory` を埋めており、
  その `GetFileAttributesEx` が失敗したときは `returnData` が NULL のまま =
  問い合わせ自体がエラーになる (`std_rc != 0`) ので取りこぼしは無い
  (`/home/hight/np21w-src/src/generic/hostdrvnt.c`)。
  **問い合わせの回数は増やしていない** — 種別は元から引いていた
  `FileStandardInformation` の応答で分かるので、`hdrv_stat` のように
  `FileBasicInformation` を追加で引くことはしなかった (get_file_size は
  open のたびに走り、hsync は何千回も呼ぶ)。
- ext2 側はすべて RAM ディスク上のホスト試験。実 NHD では走らせていない。

## 7. 2 巡目 — Codex 実装レビューの P1-1〜P1-5 と free_all_blocks

- 基点: `cce2055` (1 巡目の着地)
- 出所: Codex 実装レビュー。32bit バイナリを Unicorn で実行し、追加反例込みで
  334 件中 11 件失敗を再現。PM がコードで 6 件とも確認済み
- 実行: `python3 -B tools/tests/test_b8_open.py --target` と
  `python3 -B tools/tests/test_b8_hostdrv.py --target`
  (どちらも `make check-b8-open-host` が回す)

### 7-0. 正直に書く ([V4])

- **1 巡目の修正は「検索エラーを受け取る次の段」に届いていなかった。**
  `ext2_vfs_write` の作成経路だけ直し、同じ形の `mkdir` / `ext2_create` /
  `rename` の宛先確認を見落とした。
- 試験は今回も実装のあとに書いた。赤は変異試験で取った (§7-5)。
- **作業の途中でユーザーの PC が落ちた。** 再開時に差分を全部読み直し、
  次の 2 つを「落ちる前に終わっていなかったもの」として見つけて直した:
  1. `ext2_unlink` / `ext2_rmdir` が、ブロックを返しきれなかったときも
     `ext2_free_inode` を呼んでいた (§7-3)。**自分で書いたコメント
     (「残したブロックは辿れる」) と矛盾する論理の穴**で、ファイルの破損ではない
  2. 新しい試験 `test_b8_hostdrv.py` が `build/sdk.mk` に未登録だった
  また、再開時にスクラッチ領域の補助スクリプト (変異試験など) が消えていたので
  作り直した。作り直した変異スクリプトは**コンパイルが通らないだけの変異を
  RED と数えない**よう分けてある — 落ちる前の版では free_all_blocks の変異が
  「未使用関数で -Werror」になっていただけなのを RED と誤読していた。

### 7-1. `ext2_find_entry` の呼び出し元 (全 8 か所)

「戻り値を `== EXT2_OK` / `!= EXT2_OK` の二値に潰していないか」で分けた。

| 箇所 | 関数 | 今回の前 | 扱い |
|---|---|---|---|
| `fs/ext2_dir.c:267` | `ext2_mkdir` 存在確認 | **`== EXT2_OK` だけ拒否** | **P1-1 で修正** |
| `fs/ext2_file.c:131` | `ext2_create` 存在確認 | **`== EXT2_OK` だけ拒否** | **P1-2 で修正** |
| `fs/ext2_dir.c:537` | `ext2_rename` 宛先確認 | **`== EXT2_OK` だけ置き換え** | **P1-3 で修正** |
| `fs/ext2_vfs.c:159` | `ext2_vfs_write` 既存判定 | 1 巡目で修正済 | 3 値 (OK / NOTFOUND / その他) |
| `fs/ext2_dir.c:617` | `ext2_lookup` | 1 巡目で修正済 | `return ret` |
| `fs/ext2_file.c:379` | `ext2_unlink` | 元から `if (ret) return ret` | 問題なし |
| `fs/ext2_dir.c:367` | `ext2_rmdir` | 元から `if (ret) return ret` | 問題なし |
| `fs/ext2_dir.c:520` | `ext2_rename` 移動元 | 元から `if (ret) return ret` | 問題なし |

直し方は 3 つとも同じ: **`EXT2_OK` なら EXIST (rename は置き換え分岐)、
`EXT2_ERR_NOTFOUND` のときだけ先へ進み、それ以外は何も書かずにそのまま返す。**

### 7-2. P1-4 HostDrv — OPEN の失敗を「不存在」と区別する

**直す前**: `hostdrv_create()` が失敗を一律 `-1` にし、呼び手 11 か所のうち
7 か所が `VFS_ERR_NOTFOUND`、4 か所が `VFS_ERR_IO` に畳んでいた。
stat は成功 → サイズ取得の OPEN だけ失敗 → NOTFOUND → `vfs_open` の `O_CREAT`
が `hdrv_write_file` へ → `NP2_FILE_OVERWRITE_IF` が既存ファイルを切り詰める。

**直し方**: `hostdrv_create()` が NTSTATUS を純関数
`hdrv_create_status_to_vfs()` (`fs/hostdrv_stat_rules.inc`) で VFS エラーへ写し、
**呼び手 11 か所すべてがそのまま返す**。応答が無いとき (番兵) は IO。

**NP21/W 側の根拠** (`/home/hight/np21w-src/src/generic/hostdrvnt.c` の
`hostdrvNT_IRP_MJ_CREATE`、読むだけで変更していない):

| NTSTATUS | NP21/W が返す場面 | 写し先 |
|---|---|---|
| `OBJECT_NAME_NOT_FOUND` 0xC0000034 | 名前が無い | **NOTFOUND** |
| `OBJECT_PATH_NOT_FOUND` 0xC000003A | 途中のディレクトリが無い | **NOTFOUND** |
| `OBJECT_NAME_INVALID` 0xC0000033 | 無効な文字。**NP21/W 側のコメントに「ここで NOT_FOUND を返すとワイルドカード付き copy が壊れる」とあり、わざと分けてある** | INVAL |
| `OBJECT_NAME_COLLISION` | `FILE_CREATE` で既にある | EXIST |
| `FILE_IS_A_DIRECTORY` / `NOT_A_DIRECTORY` | 種別の不一致 | ISDIR / NOTDIR |
| `DIRECTORY_NOT_EMPTY` | 削除時 | NOTEMPTY |
| `SHARING_VIOLATION` / `ACCESS_DENIED` / `MEDIA_WRITE_PROTECTED` / `TOO_MANY_OPENED_FILES` / `CANNOT_DELETE` / `INVALID_PARAMETER` | いずれも「在るが開けない」系 | **IO (既定)** |

**既定を IO にしてあることが安全性の要** — 知らない状態を「無い」と言わない。
足りなかった 6 つの状態値は `fs/hostdrvfs_proto.h` に NP21/W の
`hostdrvntdef.h` と同じ値で足した。

**同じ畳み込みの洗い出し**: `hdrv_list_dir` / `hdrv_read_file` /
`hdrv_get_file_size` / `hdrv_read_stream` / `hdrv_stat` / `hdrv_rmdir` /
`hdrv_unlink` / `hdrv_rename` (以上 NOTFOUND に畳んでいた) と
`hdrv_write_file` / `hdrv_mkdir` / `hdrv_write_stream` (IO に畳んでいた) の
**11 か所すべて**を `return rc` にした。

**受入の形**: 純関数だけの試験では足りない (1 巡目がそれで取り逃した) ので、
**実物の `fs/hostdrvfs.c` をホストで動かす**試験
`tools/tests/b8_hostdrv_host.c` を作った。`include/io.h` の `inp`/`outp` は
特権命令のインライン asm なので `tools/tests/hostdrv_hostshim/io.h` を `-I` で
先に置いて差し替え、コマンド列 `HDR9801` が揃った時点で**贋の NP21/W** が
`g_iostatus` / `g_databuf` を書く。`hdrv_get_file_size()` を**直接**呼び、
`Directory` の値ごとの戻り値 (`ISDIR`) と、OPEN の各 NTSTATUS に対する戻り値を見る。

被害の見え方にも注意が要った: **0 バイト書き込みは `IRP_MJ_WRITE` を 1 度も
出さない**ので「WRITE 回数 0」は安全の証拠にならない。中身を消すのは
**`OVERWRITE_IF` の CREATE そのもの**なので、その回数を数える。

### 7-3. P1-5 と「最後の sync」、および削除時の inode

**P1-5 の方針**: `ext2_write_stream` は `ext2_write_inode` / `ext2_sync` の
どちらかが失敗したら **`EXT2_ERR_IO` を返し、部分成功の値は用意しない**。

理由: 正の戻り値の意味は「そのバイト数がファイルの中身として読み戻せる」で、
**長さは inode にしか無い**。inode を書けなければその約束は成り立たない。
データブロックは既に媒体に載っているかもしれないが、(a) 旧サイズの内側なら
中身の更新として正しく、(b) 外側なら inode から参照されず見えない、の
どちらかで、読める中身が壊れることはない。呼び手には「確認できなかった」
とだけ伝える。**同じ offset へ同じ内容を書き直すのは安全**なので再試行できる。

**同じ形の洗い出し** (書き込み経路の最後の `ext2_sync` / `ext2_write_inode`):

| 箇所 | 扱い | 理由 |
|---|---|---|
| `ext2_write_stream` 最後の `write_inode` / `sync` | **失敗を返す** | P1-5 |
| `ext2_create` / `ext2_write` / `ext2_mkdir` / `ext2_rename` 最後の `sync` | **失敗を返す** | この FS の約束は「戻った時点でディスクが正しい」(write-through、`ext2_sync` のコメント)。書き戻せなかったのに成功と言うのは嘘になる |
| `ext2_rmdir` 最後の `sync` | **失敗を返す** | 同上 |
| `ext2_add_entry` / `ext2_delete_entry` の途中の `write_inode` (mtime 更新) | **そのまま (返さない)** | ディレクトリブロックの書き込みは既に成功しており、エントリは媒体上で追加 / 削除済み。ここで失敗を返すと呼び手が「追加できなかった」と読んでやり直し、**二重エントリを作る**。直すなら順序を変える別の設計が要る |
| `ext2_rename` 途中の親 links / ctime 更新 | **そのまま** | 同じ理由 (名前の移動は済んでいる) |
| `ext2_create` の失敗後始末の `free_all_blocks` | `(void)` で明示 | 既に失敗を返す途中で、それ以上に良い手が無い |

`ext2_sync` は `ext2_write_super_raw` がブロック 1 を read-modify-write するので、
その**読み出し**を落とせばホスト試験で失敗させられる (`[SYNC]` の段)。

**削除時の inode (再開時に見つけた穴)**: `ext2_unlink` / `ext2_rmdir` は
`delete_entry` の**あと**でブロックを返すので、返しきれなくても名前は既に
消えている — そこで「消えていない」とは言えず、**返しきれなかったことだけを
エラーで報告する**。ただし**そのときは inode を解放しない**。
`free_all_blocks` は失敗時にブロックの指し先を inode に残すので、inode の
ビットまで空きに戻すと、次の `ext2_alloc_inode` が同じ番号を配って
`ext2_create` が inode を上書きした瞬間に**残したブロックを指すものが消える**
(本当の漏れ)。inode を「links 0・dtime 付き・使用中」で残せば孤児として
辿れ、e2fsck が回収できる。

### 7-4. `ext2_free_all_blocks` の方針

**直す前**: 間接表が読めないと内側のループを飛ばし、**表ブロック自体は解放して
ポインタを 0 にしていた** → 配下のデータブロックが使用中のまま行方不明。

**方針: 「全部返せると分かってから返す」**。先に**下見** (`ext2_free_probe`) で
単一間接表・二重間接表・その内側の表を全部読み、**1 本でも読めなければ
何ひとつ解放せずに** `EXT2_ERR_IO` を返す。戻り値は `void` から `int` に変えた。

**途中まで解放して中断してはいけない理由**: 呼び手 `ext2_write` は失敗すると
**inode を書かずに戻る**ので、媒体上の inode は解放済みブロックを指したまま
になる。そのブロックが次の割り当てで別ファイルへ渡ると、2 つの inode が
同じブロックを指す — 2026-09-06 に踏んだ相互リンクと同じ壊れ方
(gotcha §4-24)。直接ブロックを先に解放する元の順序のままでは、間接表で
失敗した時点で既に直接ブロックを返してしまっているので、**下見が必須**だった。

**代償**: 間接ブロックを持つファイル (12KB 超) の切り詰め・削除で、
**間接表の読み出しが 2 倍**になる。直接ブロックだけのファイルは下見が
何も読まないので従来どおり (`test_ext2_write_io` のセクタ数の期待値は不変)。

**残る穴 ([V4])**: 下見は通ったのに本番の解放中にだけ読めなくなった場合は
途中まで解放してしまう。一度きりの失敗なら下見が先に消費するので起きないが、
下見と本番のあいだで初めて壊れた場合は防げない。完全に閉じるにはジャーナルが要り、
本票の範囲を越える。

### 7-5. 追加試験と件数、変異

**`test_b8_open.py`: 261 → 491 checks, 0 failures** (230 件増)

| 名前 | 中身 |
|---|---|
| `case_mkdir_existence_failure` [P1-1] | `vfs_mkdir("/big/keepme")` の存在確認を一度落とす → 失敗を返す・**セクタ書き込み 0**・同名エントリ 1 件のまま・中身無事。回帰で EXIST と新規作成 |
| `case_create_existence_failure` [P1-2] | `ext2_create` を直接。同上 |
| `case_rename_dest_failure` [P1-3] | `vfs_rename("/etc/plain", "/big/keepme")` の宛先確認を一度落とす → セクタ書き込み 0・宛先が二重にならない・**移動元の名前が残る**。回帰で置き換え |
| `case_write_stream_inode_failure` [P1-5] | 5 バイトに 4 バイト追記、inode 更新の読み出しだけ落とす → 4 を返さない・サイズ 5 のまま。やり直すと **戻り値 4 と媒体上のサイズ 9 が一致**し中身も一致 |
| `case_free_all_blocks_failure` | 16KB ファイルの上書き中に間接表が読めない → **ビットマップ上で**直接ブロック・間接表・その先の実データが使用中のまま (inode は書き戻されないので inode を見ても分からない)、次に作るファイルがそれらを受け取らない、中身無事 |
| `case_unlink_keeps_inode` | 削除でブロックを返しきれない → IO を返す・名前は消える・**inode もブロックも使用中のまま**・次のファイルが同じ inode 番号を受け取らない |
| `case_rmdir_keeps_inode` | 間接ブロックを持つ空ディレクトリを作り、空判定の読み出しは通して下見だけ落とす → 同上 |
| `case_trailing_sync_failure` | スーパーブロックの読み出しを落として create / write / mkdir / 追記 / rename が IO を返すこと。回帰で全部通る |

**`test_b8_hostdrv.py`: 新規 61 checks, 0 failures** — 実物の `fs/hostdrvfs.c`

| 名前 | 中身 |
|---|---|
| E1 | `hdrv_get_file_size` 直接: 通常ファイルはサイズ、**`Directory`=1 / 2 は `ISDIR`**、QUERY 失敗は IO |
| E2 | OPEN の各 NTSTATUS: NAME/PATH_NOT_FOUND だけ NOTFOUND、SHARING_VIOLATION 等は NOTFOUND でない、NAME_INVALID は INVAL、**無応答 (番兵) は IO** |
| E3 | `hdrv_read_file` / `hdrv_stat` / `hdrv_read_stream` / `hdrv_unlink` / `hdrv_rename` も畳まない。本当に無いなら従来どおり NOTFOUND |
| E4 | 切り詰めの印 (`OVERWRITE_IF` の CREATE) の対照を取り、OPEN 失敗・無応答・ディレクトリで**それが 0 回**であること |
| E5 | `hdrv_create_status_to_vfs` の対応表そのもの |

**変異 (2 巡目の修正 19 件): 19 / 19 RED** — いずれも**コンパイルは通り**、試験が振る舞いで落とした

| 変異 | 落ちた件数 |
|---|---|
| P1-1 mkdir が I/O を素通り | 7 |
| P1-2 ext2_create 同 | 6 |
| P1-3 rename 宛先確認を「無い」扱い | 9 |
| P1-5a write_stream が inode 更新失敗を捨てる | 2 |
| P1-5b write_stream が sync 失敗を捨てる | 1 |
| SYNC-a ext2_create 最後の sync を捨てる | 1 |
| SYNC-b ext2_write 同 | 1 |
| SYNC-c mkdir / rename 同 | 2 |
| FREE-a free_all_blocks が下見の結果を無視 | 7 |
| FREE-b free_all_blocks を直す前の形に | 18 |
| FREE-c ext2_write が free の失敗を無視 | 5 |
| KEEP-a unlink が返しきれなくても inode を解放 | 5 |
| KEEP-b rmdir 同 | 1 |
| KEEP-c unlink が返しきれなかったことを報告しない | 1 |
| P1-4a hostdrv_create が失敗を一律 -1 に | 7 |
| P1-4b 呼び手 11 か所が NOTFOUND に畳む | 17 |
| P1-4c 対応表の既定を NOTFOUND に | 19 |
| P1-4d 無応答 (番兵) を NOTFOUND に | 3 |
| P1-4e get_file_size が Directory を見ない | 4 |

**1 巡目の変異 18 件も再実行して 18 / 18 RED のまま** (M1 71 / M2 37 / M3 57 /
M4 36 / M5 58 / M6 2 / M7 1 / M8 3 / M9 1 / M10 4 / M11 1 / M12 2 / M13 2 /
M14 1 / M15 1 / M16 1 / M17 2 / M18 4)。件数が §4 より増えているのは試験が
増えたため。

### 7-6. 触ったファイル (2 巡目)

`fs/ext2_dir.c` `fs/ext2_file.c` `fs/ext2_inode.c` `fs/ext2_priv.h`
`fs/hostdrvfs.c` `fs/hostdrvfs_proto.h` `fs/hostdrv_stat_rules.inc`、
`build/sdk.mk`、`tools/tests/b8_open_host.c` `tools/tests/ext2_read_bound_host.c`
(スタブの型を `int` に)、新規 `tools/tests/b8_hostdrv_host.c`
`tools/tests/test_b8_hostdrv.py` `tools/tests/hostdrv_hostshim/io.h`。
`sdk/kapi.json` 不変。NP21/W のソースは読むだけ。

### 7-7. 実機で見ていないこと ([V4])

- HostDrv の NTSTATUS の写し方は、NP21/W のソースと贋エミュレータ上の
  試験で確かめただけ。**実エミュレータで `SHARING_VIOLATION` などを起こして
  見てはいない**。
- ext2 側はすべて RAM ディスク上。実 NHD では走らせていない。
