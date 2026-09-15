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
