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
| **公開の前** (一時ファイルへの書き込み・検証・mtime・置換の途中まで) の I/O 失敗、空き不足 | 旧宛先の名前と内容が残る。一時ファイルを片づける。非ゼロ終了 |
| **公開の後** (宛先エントリの inode が新しい方を指した後) の失敗 | **検証済みの新しい内容が宛先に現れる**。後始末 (旧 inode の解放、一時名の削除) が落ちたら漏れが残り、`e2fsck` が回収する。hsync は宛先を読み直して「公開された」ことを確かめ、`replace_partial` として報告する (成功に数えない) |
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
- **段 2 が「公開」の境界** (Codex 往復 1 所見 1)。段 2 が媒体に届いた後は、段 3 以降が失敗しても
  宛先は**新しい内容**になっている。「失敗したら必ず旧内容」ではない。したがって:
  - `ext2_rename` は段 2 が成功したら、段 3〜5 を**最後まで試みてから**戻る (途中で止めない)。
    後始末に失敗したら `ext2_fs_error` でマウントを書き込み禁止にし、`EXT2_ERR_IO` を返す。
  - 1KB のブロックを 512B ずつ 2 回書く実装なので、「変更したフィールドが 1 セクタ内」は
    「失敗したら未変更」を意味しない (同上)。**前半セクタが書けた時点で公開**とみなす。
  - 呼び手 (hsync) は rename が失敗しても**宛先を読み直して**、公開されたかどうかで報告を分ける (§2-3 手順 8)。
- 段 2 の書き換えは `inode` (4 バイト、エントリ先頭) だけ。エントリは 4 バイト境界に並ぶので
  セクタ境界をまたがない。`file_type` は `S` と `D` が同じ型のときだけ段 2 に進む
  (違えば今の unlink + add の経路に落とすか `EXT2_ERR_INVAL`。票で決める → 推奨は「通常ファイル同士だけを新経路、
  それ以外は従来経路」)。
- `D` が他にも名前を持つ (`links_count > 1`) 場合は段 5 で減らすだけ。hsync はこれを §2-3 で先に断る。
- 失敗注入: `tools/tests/b8_open_host.c` の once / sticky 掃引と、コーダー票 §10-B で足す二重故障掃引に
  「ファイルの置き換え rename」を載せ、`media_check` と `e2fsck -fn` で **漏れ以外の不整合 0** を示す。
  媒体の配置は「移動元と宛先が同じブロックの前 / 後」「同じセクタ / 別セクタ」「別ブロック」に分ける
  (Codex 往復 1 所見 5)。
- **途中で止まった媒体からの復旧** (同 所見 3)。段 1〜4 の途中で電源が落ちると、名前の数より
  `links_count` が多い inode が残る (安全側だが、そのままでは収まらない):

  | 止まった位置 | 残る状態 | 次の `hsync` の挙動 |
  |---|---|---|
  | 段 1 の直後 | 一時名だけが `S` を指し `links_count=2` | 一時名は**予約された名前空間**なので無条件に消す (§2-4) → `links_count=1`、名前 0 = 漏れ。再コピーで収束 |
  | 段 2 の直後 | 宛先名と一時名が `S`、`links_count=2` | 一時名を消す → 宛先名 1・links 1 で**新しい内容として整合**。収束 |
  | 段 3 の直後 | 一時名は無く、宛先の `links_count=2` | 名前 1・links 2 = 数が多いだけ。内容は新しい。次の更新は §2-3 手順 6 の hardlink 検査に当たるので、**`reason=hardlink links_count=2` と出し、ホストの `e2fsck -fn` を案内する** (hsync が推測で links を直さない) |

  この 3 つ目だけは再実行で収まらない。**B8 §2-6 の「漏れは意図した交換」と同じ扱い**として票に明記する。

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
   日時が揃っている。**ここでの失敗は「公開の前の失敗」**なので `reason=metadata_failed` で中断し、
   一時ファイルを消す。**`copied` に数えない** (Codex 往復 1 所見 4: ext2 はメタデータの I/O 失敗で
   マウントを書き込み禁止にするので、続く rename は必ず `ROFS` になり、宛先は旧内容のまま。
   本名へ書いた後に mtime を付ける今の `apply_mtime` の集計とは前提が違う)。
   コピー元の mtime が不明 (0) のときに設定を省くのは今どおり (`mtime_unknown`)。
8. **置換**: `sys_rename(tmp, dst)`。
   - 成功: `copied`。
   - 失敗: **宛先を読み直して公開の有無を判定する** (所見 1)。宛先のサイズと CRC が新しい内容と一致すれば
     `reason=replace_partial` (公開済み・後始末が落ちた) として **errors に数え**、一時名が残っていれば消す。
     一致しなければ `reason=replace_failed` (旧内容のまま) として一時ファイルを消す。
     どちらも非ゼロ終了。読み直し自体が失敗したら `replace_unknown` (どちらか分からないと明示する)。
9. **同期**: `vfs_sync`。失敗は `reason=sync_failed` (置換は済んでいる可能性がある、と表示)。

各失敗の後始末は**この実行が作った一時ファイルだけ** (`O_EXCL` で作れた = 自分の物) を `sys_unlink`。
unlink も失敗したら `STALE <tmp>` を表示して errors に数える。

新規ファイル (宛先が無い) も同じ手順を通す。途中で落ちても**本名で半端なファイルが見えない**。

**古いカーネル** (`api->version < 53`): 一時ファイル方式は使えない (`O_EXCL` が無視され、ext2 の置き換えも
旧順序)。**既定は変更前に断る** (`reason=kernel_too_old`、1 件も書かずに非ゼロ終了)。
直接上書きが要るなら `--unsafe-overwrite` を明示する — そのときだけ今の直接上書きで進み、
`WARN kernel KAPI v52 < 53: direct overwrite (no H2)` と集計 `direct_overwrite=N` を出す
(Codex 往復 1 所見 5: 「更新の道が無くなる」は成り立たない。カーネルは停止中の NHD 配備経路で入れ替えられる)。
`-f` では解除しない。

### 2-4. `.hs~` は hsync の予約名 — 決裁 D3 (a′)

利用者の決裁は (a) 「訪れた所で自動削除」。Codex 往復 1 所見 2 が出した反例
(利用者が `/usr/bin/.hs~notes` という**自分のファイル**を置いていると消える) は到達可能なので、
**所有の根拠を「作った印」ではなく「予約された名前空間」に置く**形に直す:

- `.hs~` で始まる名前は **hsync の予約**。man ページ (`hsync.1`) と `docs/06_filesystem.md` に
  「この接頭辞のファイルは hsync が作り、消す。利用者は使わない」と明記する。
- 掃除の対象は `.hs~` で始まる**通常ファイル**。`st_nlink` は**見ない**
  (途中で止まった媒体では 2 になり得る。§2-2 の復旧表)。ディレクトリ・特殊ファイルは消さない。
- 掃除するのは `hsync` が**実際に訪れた** (同期対象の) ディレクトリだけ。既定除外の `/sys` や
  保護対象は今までどおり触らない。表示は `CLEAN <path>` (dry-run は `PLAN-CLEAN`)、集計 `cleaned=N`。
- 手順 2 で一時名が既に在ったとき: 上の条件を満たせば消してから 1 回だけ作り直す。
  ディレクトリ等で消せないなら `reason=temp_exists` で失敗。
- コピー元の列挙で `.hs~` で始まる名前は同期対象にしない (予約名を宛先へ運ばない)。

(b) 案 (消さずに `STALE` と数える) と (c) 案 (`--clean-temp` のときだけ消す) は退けた。理由は
電源断のたびに手作業が要ること。**予約を文書化することが (a) の前提**なので、文書の変更を §3 の範囲に含める。

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
| X3 | ext2 のファイル置き換え rename に once / sticky / 二重故障を全位置で。媒体の配置は「同じブロックの前 / 後」「同じセクタ / 別セクタ」「別ブロック」 | 宛先の名前が**全試行で**存在し、旧 inode か新 inode を指す。`media_check` と `e2fsck -fn` は漏れ以外 0 |
| X3b | 段 2 の前半セクタだけ成功 (1KB を 512B ずつ書く経路) | 公開済みとして扱われ、段 3〜5 が試みられる。宛先の名前は消えない |
| X4 | 置き換え後の旧 inode (links 1 → 0) の解放、links 2 → 1 | 解放される / 残る。交差リンク無し |
| A14a | 一時ファイルへの write 失敗、空き不足 (FULL)、読戻し CRC 不一致、**公開の前**の rename 失敗 | 旧宛先の内容・サイズ・mtime が不変、一時ファイル無し、errors、非ゼロ |
| A14a2 | **公開の後**に段 3〜5 が失敗 (rename は失敗を返す) | 宛先が新しい内容になっていることを hsync が読み直して確かめ、`replace_partial` で errors、非ゼロ。旧内容に戻そうとしない |
| A14a3 | 一時ファイルへの `sys_set_mtime` が I/O 失敗 → マウントが書き込み禁止 (`ROFS`) → rename が拒否される | `metadata_failed`、**`copied` は 0**、宛先は旧内容のまま、一時ファイル無し |
| A14b | 一時名が既に存在 (通常ファイル / ディレクトリ / `st_nlink=2`) | 通常ファイルは消して作り直す、ディレクトリは `temp_exists` で失敗、`st_nlink` では判断しない |
| A14b2 | 利用者が `.hs~notes` という自分のファイルを置いている | **消える** (予約名。man ページに明記されていることを試験の前提として記録する) |
| A14c | 一時ファイルの unlink も失敗 | `STALE` 表示、errors |
| A15 | hsync の各 I/O の直後で「停止」(以降の I/O を全部失敗) → 再マウント相当 → 再実行 | 宛先は旧か新のどちらか完全な内容。§2-2 の復旧表の 3 状態を**個別に**通す: 段 1 直後 (一時名 + links 2) と段 2 直後 (2 名 + links 2) は再実行で収束、段 3 直後 (links だけ 2) は `hardlink` で断り `e2fsck` を案内する |
| A15b | A15 の各状態のあとでコピー元が更に更新された | 同じ判定。宛先を壊さない |
| A15c | `create_excl` が名前を作った直後に `vfs_sync` / FD 発行が失敗 | 一時名だけが残る。次の実行が予約名として片づける |
| A17a | 書き込み中にコピー元のサイズ / mtime が変わる | `source_changed`、置換しない |
| A17b | 判定後に宛先が消える / 型が変わる / サイズか mtime が変わる / 保護対象になる | `dest_changed` (保護は `settings_db`)、置換しない |
| A17c | 宛先の `st_nlink > 1` | `hardlink`、一時ファイルを作らない |
| R1 | 古いカーネル (version 52)、`--unsafe-overwrite` 無し | 1 件も書かずに `kernel_too_old` で非ゼロ終了 |
| R1b | 同、`--unsafe-overwrite` あり + コピー元の read が途中で失敗 | `WARN` 1 行、直接上書き、`direct_overwrite=N`、**旧内容は失われる**ことを表示と文書で明示 |
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
| D1 | 排他的作成の口 | (a) `O_EXCL` を KAPI フラグで、v53 | **(a) ユーザー決裁 2026-09-16** |
| D2 | 置換の手段 | (a) ext2 の置き換えを宛先エントリの inode 書き換えに | **(a) ユーザー決裁 2026-09-16** |
| D3 | 残った一時ファイル | (a) 訪れたディレクトリで自分の形のものだけ消す | **(a) ユーザー決裁 2026-09-16**。Codex 往復 1 所見 2 を受け、所有の根拠を「予約名 `.hs~`」に置き換えて (a′) に精密化 (§2-4) |

## 6. 往復記録

### 往復 1 — 設計レビュー (Codex、`34cfc3f` 対象、2026-09-16) — Request changes

| # | 重大度 | 所見 | PM の確認 | 対応 |
|---|---|---|---|---|
| 1 | blocker | 段 2 の後に失敗すると宛先は**新内容**になる。「失敗したら旧内容」は成り立たない。1KB を 512B ずつ書くので「1 セクタ内のフィールド」も未変更を保証しない | 到達可能。`ext2_write_block` の実装と一致 | §0 の保証表を**公開の前 / 後**に分割。段 2 を公開境界と定義し、公開後は段 3〜5 を最後まで試みる。hsync は rename 失敗時に宛先を読み直して `replace_partial` / `replace_failed` / `replace_unknown` を分ける (§2-2、§2-3 手順 8、A14a2、X3b) |
| 2 | blocker | `.hs~` + `nlink==1` では所有を証明できない。利用者の `.hs~notes` が消える | 到達可能 | 所有の根拠を**予約名**に変更 (§2-4 (a′))。man ページと `06_filesystem.md` に予約を明記し、`st_nlink` は見ない。受入 A14b2 を追加 |
| 3 | blocker | 段 1〜3 の途中で止まると `links_count` が名前の数より多く残り、再実行で収束しない (掃除条件・hardlink 検査に当たる) | 到達可能。3 状態とも再現する | §2-2 に復旧表を追加。一時名は `st_nlink` を見ずに消す (段 1・2 の状態は収束)。段 3 の状態は `hardlink` で断り `e2fsck` を案内する (B8 §2-6 の「漏れ」と同じ扱い)。A15 を 3 状態に分割 |
| 4 | blocker | 一時ファイルへの mtime 失敗は **ROFS** を招き rename が通らない。`copied` に数えられない | 到達可能 (`fs/ext2_super.c` のエラー状態) | §2-3 手順 7 を「公開前の失敗として中断、`copied` に数えない」に変更。A14a3 を追加 |
| 5 | 非blocker | 旧カーネルでの直接上書きは旧内容を失う。「更新の道が無くなる」は成り立たない (停止中の NHD 配備がある) | 妥当 | 既定を**拒否** (`kernel_too_old`) に変更し、`--unsafe-overwrite` を明示したときだけ直接上書き。R1 / R1b |

観点 1〜5 すべて「見た」。媒体の故障注入と実機試験は未確認 (実装段階の受入で行う)。
