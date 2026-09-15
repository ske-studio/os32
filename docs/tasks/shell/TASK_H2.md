# TASK_H2 — hsync の置換安全化 (一時ファイル + 検証 + 置換)

> 発行: PM (Claude Code `claude-opus-5`、2026-09-16) / 状態: **設計中 (2026-09-16)**

基点: `feat/gui` = `d191fd5`。
計画の正典: [`HSYNC_IMPROVEMENT_PLAN.md`](HSYNC_IMPROVEMENT_PLAN.md) §6 (手順と保証の限界)、§8 (H2 の行)、§9 (A14 / A15 / A17)。
引き継ぎ: [`../agents/HANDOVER_2026-09-16.md`](../agents/HANDOVER_2026-09-16.md) §3。
前提の票: [`TASK_H1.md`](TASK_H1.md) (内容比較・CRC 読戻し)、[`TASK_H3.md`](TASK_H3.md) (mtime)、
[`TASK_FS_TYPE.md`](TASK_FS_TYPE.md) (B8: 読めなかったを無いと読まない、ext2 の書き込み順序)。

## 0. 目的

**新内容の書き込みと検証が済むまで旧宛先を残す。**
今の `hsync` は宛先を `O_WRONLY | O_CREAT | O_TRUNC` で開いて直接上書きする
(`userland/system/hsync.c` の `copy_verify`)。書き込み途中・検証・同期のどこで失敗しても
旧内容は既に切り詰められていて戻らない (失敗行に「直接上書きなので旧内容は残らない」と出している)。

保証する範囲と、しない範囲:

| 事象 | H2 の保証 |
|---|---|
| 書き込み・読戻し検証・同期・置換の **I/O 失敗、空き不足** | 旧宛先の名前と内容が残る。自分が作った一時ファイルだけを片づける。非ゼロ終了 |
| コピー中の **コピー元 / 宛先の変化** (stat で見えるもの) | 置換しない (`source_changed` / `dest_changed`)。同サイズ・同時刻へ戻る変化は捕まえない (計画 §6) |
| **電源断・エミュレータ強制終了** | 宛先の名前は旧 inode か新 inode のどちらかを指す (§2-2 の順序による)。**一時ファイルが残り得る**。後始末は §2-4 の決裁。ジャーナルは無いので「原子的」とは書かない |
| 異なる FS をまたぐ置換、複数ファイルの一括切り替え | 対象外 |

## 1. 確認した事実 (2026-09-16、`d191fd5`)

1. **排他的作成が無い**。open フラグは `KAPI_O_RDONLY / WRONLY / RDWR / CREAT / TRUNC` の 5 つ
   (`sdk/include/os32/os32_kapi_shared.h:477-488`)。`wrap_sys_open` はフラグを素通しで `vfs_open` へ渡す。
   `vfs_open_internal` (`fs/vfs_fd.c`) は「`get_file_size` が NOTFOUND なら `write_file(path, "", 0)` で作る」。
   知らないビットは黙って無視される。
2. **ext2 のファイル置き換えは宛先を先に消す**。`ext2_rename` (`fs/ext2_dir.c:828`) は宛先が
   通常ファイルなら `ext2_unlink(new)` → `links_count++` → `ext2_add_entry(new)` → `ext2_delete_entry(old)`
   → `links_count--` の順。unlink の後で落ちると**宛先の名前が無い状態**が残る。
   ディレクトリ側 (`ext2_rename_dir`) は B8 で「旧名を先に消す」に決裁済みで、本票の対象外。
3. `sys_rename` の利用者は `userland/shell/cmd_file.c:263` (`mv`) と `userland/system/install.c:89`。
   SQLite の VFS (`lib/sqlite3/os32_sqlite_vfs.c`) は rename を使っていない。
   §2-2 の変更はこの 2 者の挙動も (より安全な方へ) 変える。
4. VFS の FD は**パスで** FS を指す (`open_files[fd].path`)。置換で inode が替わると、開いている FD の
   続きの read は新しい内容を読む。今の unlink + add でも同じなので、H2 で新しく生まれる問題ではない。
5. `OS32_Stat` に `st_nlink` がある (`os32_kapi_shared.h:463`)。
6. KAPI は v52。アプリは `api->version` で版を見られる (`userland/tests/host_test.c:48` に前例)。
7. `hsync` は `/boot` も同期できる (`g_touched_boot`)。**古いカーネルで新しい hsync を動かす**場面
   (カーネルを入れ替える途中) は現に起こる。

## 2. 設計

### 2-1. 排他的作成 `O_EXCL` — 決裁 D1

**推奨 (a)**: `KAPI_O_EXCL` (`0x0400`) を足し、KAPI を **v53** にする。

- `vfs_open_internal`: `O_EXCL` は `O_CREAT` と組でだけ有効 (単独は `VFS_ERR_INVAL`)。
  名前が**何であれ存在すれば** `VFS_ERR_EXIST` (ディレクトリでも ISDIR ではなく EXIST)。
  判定できない (NOTFOUND 以外の負値) なら**その値を返す** (B8)。
- 排他性の根拠: VFS は非再入で、ゲストは協調型 (GetMessage 方式) なので、1 回の `sys_open` の中の
  「無いことの確認 → 作成」に他のゲスト処理は割り込まない。**ホスト側が同時に書ける FS では成り立たない**。
- そのため `VfsOps` の末尾に任意フック `create_excl(ctx, path)` を足し、**持つ FS だけが `O_EXCL` を受ける**。
  ext2 は実装する (内部で `find_entry` → NOTFOUND のときだけ作成)。HostDrv / FAT / ISO9660 は持たず、
  `O_EXCL` は `OS32_ERR_NOSYS`。`VfsOps` はカーネル内部の構造体で ABI ではない (H3 の `set_mtime` と同じ扱い)。
- `KAPI_O_EXCL` は `os32_kapi_shared.h` の手書き定数 (生成物ではない)。`kapi.json` の構造体は変わらないが、
  **`sys_open` の意味が広がる**ので版数を上げる ([ABI3]、手順はスキル `os32-kapi-add`)。

(b) 案: hsync 専用の `sys_create_temp(dir, prefix, out_name)`。汎用性が無く、名前の規則をカーネルに持ち込むので推奨しない。

### 2-2. ext2 のファイル置き換えを「宛先の名前を消さない」順にする — 決裁 D2

**推奨 (a)**: 宛先のディレクトリエントリの **inode 番号をその場で書き換える** (Linux の `ext2_rename` と同じ考え方、コードは参照しない)。

新しい順序 (不変条件: **どの inode も名前の数 ≤ `links_count`**、B8 往復 4 と同じ):

| 段 | 書くもの | ここで落ちたときの媒体 |
|---|---|---|
| 1 | 移す inode `S` の `links_count++` | 名前 1、links +1 (多い側 = 安全)。宛先 `D` は無傷 |
| 2 | 宛先エントリの inode 番号を `D` → `S` に書き換える (1 セクタ) | 書けていない: 段 1 と同じ。書けた: 宛先名 → `S`、`D` は名前 0・links 1 (漏れ、e2fsck が回収) |
| 3 | 旧名 (移動元) を消す (`ext2_delete_entry`) | `S` の名前 2 か 1、links +1 = 整合 |
| 4 | `S` の `links_count--` | `S` の links が 1 多い (孤児側) |
| 5 | `D` の `links_count--`、0 なら `ext2_unlink` の後半と同じ解放 (参照を消してから解放、B8 往復 3) | 漏れ |
| 6 | `ext2_sync` | — |

- **宛先の名前はどの段でも消えない** (旧 inode か新 inode を指す)。これが H2 の I/O 失敗時保証の土台。
- 段 2 の書き換えは `inode` (4 バイト、エントリ先頭) だけ。エントリは 4 バイト境界に並ぶので
  セクタ境界をまたがない。`file_type` は `S` と `D` が同じ型のときだけ段 2 に進む
  (違えば今の unlink + add の経路に落とすか `EXT2_ERR_INVAL`。票で決める → 推奨は「通常ファイル同士だけを新経路、
  それ以外は従来経路」)。
- `D` が他にも名前を持つ (`links_count > 1`) 場合は段 5 で減らすだけ。hsync はこれを §2-3 で先に断る。
- 失敗注入: `tools/tests/b8_open_host.c` の once / sticky 掃引と、コーダー票 §10-B で足す二重故障掃引に
  「ファイルの置き換え rename」を載せ、`media_check` と `e2fsck -fn` で **漏れ以外の不整合 0** を示す。

(b) 案: ext2 はそのまま、hsync が「宛先を `.old` に rename → 一時を本名に rename → `.old` を消す」。
2 回目の rename の前後で**宛先の名前が無い瞬間**が残り、`.old` の後始末も増える。推奨しない。

(c) 案: ext2 はそのまま、hsync の復旧契約に「宛先が消えていたら一時ファイルから戻す」を入れる。
`mv` と `install` は守られないまま。推奨しない。

### 2-3. hsync の新しい手順 (dry-run 以外、コピーが要ると決まった後)

計画 §6 の 6 段を具体化する。既存の判定順 (H1 / H3: サイズ → mtime 前置 → 内容比較) は**変えない**。

1. **一時名**: 宛先と同じディレクトリに `.hs~<名前>`。名前が長すぎて一時名が `EXT2_NAME_LEN` を越えるなら
   `reason=name_too_long` で失敗 (errors)。コピー元の列挙で `.hs~` で始まる名前は**同期対象にしない**。
2. **作成**: `sys_open(tmp, O_WRONLY | O_CREAT | O_EXCL)`。`EXIST` なら §2-4 (決裁 D3)。
   `NOSYS` (宛先 FS が排他的作成を持たない) なら `reason=replace_unsupported` で失敗し、**直接上書きへ黙って落ちない**。
3. **書き込み + CRC**: 今の `copy_verify` の本体を一時ファイルへ。short write は進め、0 進捗・負値は失敗。
   `OS32_ERR_FULL` は `reason=no_space`。
4. **同期 → 読戻し検証**: `vfs_sync` → 一時ファイルを開き直して長さ・CRC を照合 (今と同じ)。
5. **再確認**: コピー元を stat し直し、サイズと mtime が手順開始時と同じか (`source_changed`)。
   宛先を stat し直し、「判定時に無かった → まだ無い」「判定時に通常ファイル → まだ通常ファイルで、
   サイズ・mtime が判定時と同じ」か (`dest_changed`)。**保護対象の判定 (`dst_protected`) もここで取り直す**。
6. **hardlink**: 宛先の `st_nlink > 1` なら `reason=hardlink` で失敗 (計画 §6、別名まで更新する仕様を持ち込まない)。
   判定は手順 5 と同時に、**一時ファイルを作る前にも**行う (無駄な書き込みを避ける)。
7. **mtime**: 一時ファイルに `sys_set_mtime` (H3)。rename は inode の mtime を変えないので、本名に現れた時点で
   日時が揃っている。失敗は今と同じく `metadata_failed` (コピーは成功扱い、終了コードは非ゼロ)。
8. **置換**: `sys_rename(tmp, dst)`。失敗なら `reason=replace_failed`、一時ファイルを消す。
9. **同期**: `vfs_sync`。失敗は `reason=verify_failed` (置換は済んでいる可能性がある、と表示)。

各失敗の後始末は**この実行が作った一時ファイルだけ** (`O_EXCL` で作れた = 自分の物) を `sys_unlink`。
unlink も失敗したら `STALE <tmp>` を表示して errors に数える。

新規ファイル (宛先が無い) も同じ手順を通す。途中で落ちても**本名で半端なファイルが見えない**。

**古いカーネル** (`api->version < 53`): 一時ファイル方式は使えない (`O_EXCL` が無視され、ext2 の置き換えも
旧順序)。黙って直接上書きには落とさず、開始時に 1 行 `WARN kernel KAPI v52 < 53: direct overwrite (no H2)` を出して
**今の直接上書き**で続ける。理由: カーネルを入れ替える途中の `hsync` を止めると更新の道が無くなる。
`-f` の有無で変えない。集計行にも `direct_overwrite=N` を出す。

### 2-4. 残った一時ファイルの扱い — 決裁 D3

電源断・強制終了のあとに `.hs~<名前>` が残り得る。

**推奨 (a)**: `hsync` が**訪れたディレクトリ**で、`.hs~` で始まり、通常ファイルで `st_nlink == 1` のものを
消して `CLEAN <path>` と表示する (dry-run では `PLAN-CLEAN`)。手順 2 で `EXIST` に当たったときも、
同じ条件を満たせば消してから 1 回だけ作り直す。条件を満たさなければ `reason=temp_exists` で失敗。
`/sys` の既定除外・保護対象の規則はそのまま効く (除外したディレクトリは掃除もしない)。

(b) 案: 消さずに `STALE <path>` と数えるだけ (利用者が消す)。安全側だが、電源断のたびに手作業が要る。

(c) 案: `--clean-temp` を明示したときだけ消す。

## 3. 範囲

| 層 | ファイル | 変更 |
|---|---|---|
| KAPI | `sdk/include/os32/os32_kapi_shared.h`、`sdk/kapi.json` (版数)、`docs/KAPI_SPEC.md` | `KAPI_O_EXCL`、v53 |
| VFS | `fs/vfs.h`、`fs/vfs_fd.c` | `VfsOps.create_excl`、`O_EXCL` の分岐 |
| ext2 | `fs/ext2_dir.c`、`fs/ext2_vfs.c`、`fs/ext2_priv.h` | 置き換え順序 (§2-2)、`create_excl` |
| hsync | `userland/system/hsync.c`、`docs/manpages/hsync.1` | §2-3、§2-4、理由コード追加 |
| 試験 | `tools/tests/hsync_h2_host.c` + `hsync_h2_tdd.md`、`tools/tests/b8_open_host.c`、`tools/tests/vfs_excl_host.c` (名前は任意) | §4 |
| 文書 | `docs/POLICY_DEBUG.md` §4-36、`CLAUDE.md:157`、`docs/06_filesystem.md` (rename と O_EXCL の契約) | 制限の書き換え |

外部リポジトリ (`apps/` `game/`) は `make external` で組み直すだけ ([ABI3])。

## 4. 受入

### 4-1. ホスト試験 (コーダー、RED → GREEN を `*_tdd.md` に)

| ID | 反例 | 期待 |
|---|---|---|
| X1 | `O_CREAT | O_EXCL` で既存ファイル / 既存ディレクトリ / 判定不能 (I/O 失敗) | EXIST / EXIST / その負値。**作らない・切り詰めない** |
| X2 | `O_EXCL` 単独、`create_excl` を持たない FS | INVAL / NOSYS |
| X3 | ext2 のファイル置き換え rename に once / sticky / 二重故障を全位置で | 宛先の名前が**全試行で**存在し、旧 inode か新 inode を指す。`media_check` と `e2fsck -fn` は漏れ以外 0 |
| X4 | 置き換え後の旧 inode (links 1 → 0) の解放、links 2 → 1 | 解放される / 残る。交差リンク無し |
| A14a | 一時ファイルへの write 失敗、空き不足 (FULL)、読戻し CRC 不一致、`vfs_sync` 失敗、rename 失敗 | 旧宛先の内容・サイズ・mtime が不変、一時ファイル無し、errors、非ゼロ |
| A14b | 一時名が既に存在 (自分の形 / ディレクトリ / hardlink) | D3 の規則どおり |
| A14c | 一時ファイルの unlink も失敗 | `STALE` 表示、errors |
| A15 | hsync の各 I/O の直後で「停止」(以降の I/O を全部失敗) → 再マウント相当 → 再実行 | 宛先は旧か新のどちらか完全な内容。再実行で収束し、一時ファイルが D3 どおり扱われる |
| A17a | 書き込み中にコピー元のサイズ / mtime が変わる | `source_changed`、置換しない |
| A17b | 判定後に宛先が消える / 型が変わる / サイズか mtime が変わる / 保護対象になる | `dest_changed` (保護は `settings_db`)、置換しない |
| A17c | 宛先の `st_nlink > 1` | `hardlink`、一時ファイルを作らない |
| R1 | 古いカーネル (version 52) | `WARN` 1 行 + 直接上書き、`direct_overwrite=N` |
| R2 | H1 / H3 のホスト試験全部、`test_hsync_protect.py` | 変更なしで通る |
| R3 | 新規ファイル | 一時名経由で作られ、途中失敗で本名が現れない |

### 4-2. ゲスト受入 (PM、[D1])

1. `make clean` → `make all` → `make external` → `make check`。
2. NP21/W 停止 → `os32-cycle deploy` → 起動 → `ver` で API v53、kselftest を新しい `kernel.map` の番地で読む。
3. `make deploy` → `hsync -n` → `hsync`: 変更の無い全体同期の所要時間が H3 (全体 2.9 s、`hsync sys` 0.26 s) から
   目立って悪化していないこと (一時ファイルはコピーが要るものにしか作らない)。ゲストの `time` で測る。
4. `libos32gui.shlib` 級 (約 110KB) を 1 本差し替えて `hsync sys` → `ls -l` のサイズ、`/host` 側との CRC 一致、
   `.hs~` が残っていないこと。
5. 空き不足: 宛先ボリュームの空きより大きいファイルを配備元に置いて `hsync` → `no_space`、**旧宛先のサイズ不変**。
6. (承認が要る、任意) 大きいファイルのコピー中に NP21/W を強制終了 → 起動 → 宛先が旧か新の完全な内容、
   ホストで `e2fsck -fn` (NHD を停止中に取り出す)。**NHD の作業イメージを先にバックアップする ([D2])**。

## 5. 決裁の記録

| ID | 問い | 推奨 | 決裁 |
|---|---|---|---|
| D1 | 排他的作成の口 | (a) `O_EXCL` を KAPI フラグで、v53 | 未 |
| D2 | 置換の手段 | (a) ext2 の置き換えを宛先エントリの inode 書き換えに | 未 |
| D3 | 残った一時ファイル | (a) 訪れたディレクトリで自分の形のものだけ消す | 未 |

## 6. 往復記録

(設計レビュー・実装レビューの記録をここに足す)
