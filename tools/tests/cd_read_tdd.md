# CD の読み — 複数セクタの READ(10)・覚えたパス・先読みの窓 (ホスト試験の記録)

- 症状: 実機 **PC-9821Ra266** で CD からの `cdinst` (Full) が 10 分を超えても NORMAL.PKG の途中。
  パッケージは合計およそ 12MB (BOOT 6KB・MINIMAL 1.2MB・GUI 0.4MB・NORMAL 9.7MB・DEBUG 1.3MB、
  **全部 `lzss: false`** = `pkg_extract` の無圧縮の経路) なので、実効は 20KB/s を切っている。
  NP21/W は CD のシークも回転も模擬しないので、エミュレータでは再現しない
- 見立て (机上、**実機では測っていない**):
  1. `drivers/atapi.c` の `atapi_read_sectors` は count がいくつでも READ(10) を 1 セクタずつ出していた
  2. `fs/iso9660.c` の `read_stream` は呼ばれるたびに `iso_resolve_path` で根からたどり直し
     (根のディレクトリ = LBA 23 を 1 回読む)、それから 1 セクタずつ `iso_read_sector` を呼んでいた
  3. `userland/lib/rt/pkg.c` の無圧縮の展開は 4KB ずつ `sys_read` し、データ部の先頭 (表の後ろ) が
     セクタに揃っていないので 4KB の読みは 3 セクタに掛かる
  → 4KB ごとに「LBA 23 → データ (LBA 1440〜) の 3 セクタ」の **1 セクタの READ(10) が 4 回、うち 2 回は
  離れた位置へのシーク**。12MB で約 3000 区切り × 4 = 約 12000 コマンド、シーク約 6000 回。
  `pkg_parse` (事前検査と展開で 2 回) は 1B・パス長・5B の小さな `sys_read` を項目ごとに 3 回出すので、
  1 本の PKG で数百回ぶん「根を読む + データを 1 セクタ読む」を繰り返していた
- 対象: [`drivers/atapi.c`](../../drivers/atapi.c) / [`atapi.h`](../../drivers/atapi.h)、
  [`fs/iso9660.c`](../../fs/iso9660.c) / [`iso9660.h`](../../fs/iso9660.h)、
  [`userland/lib/rt/pkg.c`](../../userland/lib/rt/pkg.c)。KAPI は動かしていない
- 実行: `python3 -B tools/tests/test_cd_read.py [--target] [--mutate] [case ...]`
  (`make check-cd-read-host` が `--target --mutate` 付きで回す)

## 1. 直したもの

### atapi.c
- `atapi_read_sectors` は連続する count セクタを **`ATAPI_READ_MAX_SECTORS` (既定 16 = 32KB) ずつの
  READ(10)** で読む。CDB の転送長 (bytes 7-8) に n を入れる
- 複数セクタの READ(10) が失敗したら (ERR、または**受け取ったバイト数がちょうど n × 2048 でない**)、
  その範囲だけ 1 セクタずつ読み直す。1 セクタでも落ちたらそこで失敗を返す
- PIO: byte count limit には `min(バッファ, ATAPI_PIO_BCL_MAX = 0xF800)` を書く (偶数・セクタの倍数、
  64KB を書くと 16 ビットに入らず 0 になる)。データは DRQ ごとに Cylinder Low/High のバイト数を読む
  (以前から複数 DRQ のループはあった)。**各 DRQ のブロックの後に ALT_STATUS の空読みで 400ns** 置く
  — 置かないと前のブロックの DRQ=1 を読んで次のブロックと取り違える
- エラーのときエラーレジスタの bit7-4 (センスキー) を覚える。UNIT ATTENTION (6) / NOT READY (2) で
  **媒体の世代** (`atapi_media_gen`) を進め、UNIT ATTENTION のコマンドは 1 回だけ出し直す
  (入れ替え直後の mount の PVD 読みが落ちないように)
- 相性で困ったら `ATAPI_READ_MAX_SECTORS` を 1 にすれば旧来の読み方 (`#ifndef` なので -D でも変えられる)

### iso9660.c
- **覚えたパス 1 本** (`pc_path` → LBA・サイズ・フラグ)。同じパスなら根からたどらない。
  見つからなかったパスは覚えない
- **ディレクトリのセクタの LRU** (4 本 = 8KB、ctx の中)。`list_dir` は cb を呼ぶ前に手元へ写す
- **先読みの窓** (16 セクタ = 32KB、mount で kmalloc、取れなければ窓なし): ファイルのデータは窓を通す。
  窓より大きいセクタに揃った範囲は呼び手のバッファへ直接 (atapi が 16 セクタずつに分ける)。
  窓はファイルの外へ広げない (媒体の最後のファイルで読めない先を読まない)。窓は読む前に捨てる
- 捨てる合図: umount (ctx ごと)、媒体の世代が進んだとき、**最後に媒体を読んでから 2 秒
  (`ISO_IDLE_TICKS` = 200 tick) を超えたとき** (キャッシュの当たりは時刻を進めない)。
  読みの途中で世代が進んだら捨てて 1 回だけ読み直す。約束は docs/06_filesystem.md §6-6
  (「CD を入れ替えたら umount / mount」)

### pkg.c (cdinst の無圧縮の展開)
- 区切りを **32KB** (`PKG_STREAM_CHUNK`、`mem_alloc`。取れなければスタックの 4KB) にし、
  **区切りの終わりをファイルの中のセクタ境界 (2048 の倍数の位置) に揃える**。最初の読みだけ半端で、
  以後は 32KB の揃った読み = iso9660 が直接 1 回 (16 セクタ) の READ(10) で読む
- 失敗の枝でも取ったバッファを返す (以前は `return` が 3 か所に散っていた)
- `cdinst.c` は触っていない (展開は `pkg_extract` が全部持つ)。LZSS の経路は元から全体を 1 回で読む

## 2. 試験

ハーネスは 2 本。どちらも実物を `#include` し、ASan/UBSan を付けて組む。

- [`cd_read_host.c`](cd_read_host.c): 実物の `atapi.c` + `iso9660.c`。ポートは
  [`atapi_hostshim/io.h`](atapi_hostshim/io.h) が **ATAPI デバイスの模型**へ回す。模型は PACKET → CDB 6 ワード →
  DRQ ごとのデータをレジスタの粒度で写し、READ(10) の数・複数セクタの数・転送セクタ数・(LBA, 数) の列を持つ。
  DRQ の大きさ (2048 = NP21/W / bcl どおり / 6144 / 1000 / 512)、複数セクタだけ失敗、ある LBA が失敗、
  複数セクタで 1 セクタ足りない転送、次のコマンドで UNIT ATTENTION + 媒体の入れ替え、を選べる。
  ブロックの境目の後は **3 回ぶん古い状態 (DRQ=1) を見せ、データレジスタは屑**を返す (400ns の整定の再現)。
  媒体は試験が組む ISO 9660 (PVD・根・2 セクタの SUB・BIG.PKG = 601 セクタで末尾は半端・媒体の最後)
- [`cd_pkg_host.c`](cd_pkg_host.c): 実物の `pkg.c` の `pkg_extract`。KAPI の贋物が `sys_read` の
  (位置, 長さ) を記録し、書き出しの中身を突き合わせる。記録を cd_read_host の `replay` で再生して、
  pkg.c の読み方のまま実物の iso9660 + atapi で READ(10) を数える
- `ATAPI_READ_MAX_SECTORS=32` の版 (1 回 64KB) でも `stream_4k` など 5 ケースを回す

| ケース | 見るもの |
|---|---|
| `stream_4k` | BIG.PKG を 4KB ずつ read_stream: 中身一致、**パスの解決 1 回**、**READ(10) ≤ ⌈601/16⌉ + 3** (実測 39)、根のセクタは 1 回、同じセクタを読み直さない |
| `stream_odd` | 1000B ずつ・13B ずれた位置から: 同じ上限 (実測 39) |
| `stream_32k` | pkg.c の形 (揃えて 32KB): 上限、窓を通さない直接の読みがある (実測 39) |
| `read_file` | 全体・切り詰め・ディレクトリ・無いファイル (実測 40) |
| `multi_fallback` | 複数セクタが全部落ちる: 1 セクタずつで中身一致 |
| `short_transfer` | 複数セクタで 1 セクタ足りない: 失敗として読み直す |
| `bad_outside` | 窓の先読みが**要求の外**の不良で落ちても、要求の範囲は読める (半端な位置からも)。窓は無効のまま、捨てた窓を当てない |
| `bad_inside` | **要求の中**の不良は VFS_ERR_IO (窓経由も、窓を通さない 64KB の読みも)。読み直しは不良で止まる |
| `stat_swap` | 解決の途中で交換: キャッシュの A の根が SUB = 21 と答え、SUB の読みが UNIT ATTENTION → B の 21 (囮) を読む。世代を見て B の根から引き直し、B の本物 (24) の DEEP.BIN の大きさを返す (stat / get_file_size) |
| `list_swap` | list_dir: 解決の途中の交換は引き直す / 2 セクタ目の読みで交換 (囮の 2 セクタ目は空 = cb が呼ばれない) → VFS_ERR_IO / cb の中の読みが交換を踏む (1 セクタの根) → VFS_ERR_IO、次の操作は新しい媒体 |
| `lru_order` | LRU の追い出しの順 |
| `ua_mount` | UNIT ATTENTION 直後の mount (PVD) が通り、世代が 1 進む |
| `multi_drq` | DRQ の区切り 5 通りで中身一致、bcl は偶数・0 でない・0xF800 以下 |
| `path_cache` | 違うパスは引き直す、同じなら引かない、見つからないパスは覚えない、mount し直しで引く |
| `dir_lru` | 2 本のパスを交互に小さく読む: パスは毎回引くがディレクトリのセクタは媒体から 1 回ずつ |
| `list_reentrant` | 窓なしで list_dir の cb が同じ FS を読み、根のスロットが追い出されても一覧が崩れない |
| `unit_attention` | 読みの途中で UNIT ATTENTION + 入れ替え: 新しい媒体の中身を返す (パスを引き直す) |
| `idle_rule` | 2 秒ちょうどは捨てない、超えたら捨てる。UNIT ATTENTION の無い入れ替えでも古い窓を当てない |
| `no_window` | 窓が取れなくても中身一致 |
| `pkg:aligned_chunks` | 読みは 32KB 以下、終わりはセクタ境界かファイルの終わり、取ったバッファを 1 回返す |
| `pkg:nomem` / `pkg:open_fail_frees` | 4KB に落ちる / 失敗の枝でもバッファを返す |
| `np2_read` / `np2_async` | NP21/W の写し (同期 / CD_ASYNC の BSY) の上で、容量・LBA 16・16 セクタ・37 セクタ・mount と読み |
| `np2_empty` | 空のドライブ: 容量 1、READ(10) 失敗と `[atapi] ... sense=5` の行、媒体が入れば読める |
| `np2_slave` | マスター CD / マスター空 CD + スレーブ CD / スレーブだけ / マスター CD + スレーブ空: 媒体のある方を選んで読む |
| `cap_nodata` | READ CAPACITY がデータ無しで終われば失敗 |
| `replay` | pkg.c の読み方 (28 回、559KB) を再生: **READ(10) 19 回** (274 セクタ ÷ 16 = 17.2 + 根 + 端)、パスの解決 1 回 |

参考 (机上): 直す前の形 (4KB ずつ、根から引き直し、1 セクタずつ) では、同じ 559KB で
4KB の区切り 144 回 × (根 1 + データ 2〜3) ≒ 550 回の READ(10)、うち約 290 回は離れた位置へのシーク。

## 3. 否定側 (変異)

`--mutate` は一時の木の写しに 1 か所ずつ当てる (ソースは書き換えない)。RED = どれかのケースが落ちた、
ERROR = コンパイルできなかった (**RED に数えない**)、SURVIVED = 見逃し。最後の 1 本は何も変えない対照で、
SURVIVED でなければ試験が不安定。変異の一覧と結果は `test_cd_read.py` の `MUTATIONS` と実行時の出力
(`MUTATION n/n RED`) が正。

- 「直す前の姿」は変異 1 (READ(10) を 1 セクタずつ) と変異 10 (覚えたパスを使わない) で、どちらも RED
  (READ(10) の上限と、パスの解決 1 回で落ちる)
- 組んでいて気づいた等価な変異は入れていない:
  - 窓の当たりの `lba >= ra_lba` — 符号無しの差で同じになるので条件から外した
  - `bcl &= ~1` — 呼び手のバッファは 8 か 2048 の倍数で、奇数にならない
  - UNIT ATTENTION の出し直しは、読みの経路では「1 セクタずつの読み直し」と iso9660 の読み直しが
    吸うので、落ちるのは mount (1 セクタの PVD 読み) だけ。`ua_mount` がそれを見る

## 3-2. Codex レビュー 1 (6e4bd83) の P2 × 2 の直し

- **P2-1** 窓の先読み (要るセクタから 16 本) が、要求の外にある不良セクタで落ちると、要求の範囲が読めても
  VFS_ERR_IO を返していた。→ 先読みが落ちたら窓は無効のまま、その呼び出しの残りは**窓なしと同じ経路**
  (揃った範囲は直接、端は LRU) で要るセクタだけを読む。`bad_sector` (この失敗を正解として固定していた) を
  `bad_outside` / `bad_inside` に分けた
- **P2-2** stat / get_file_size / list_dir は入口でしか世代を見ていなかった。旧媒体のキャッシュで得た
  サブディレクトリの LBA を読む READ(10) が UNIT ATTENTION → 出し直しで新媒体の同じ LBA を読み、その答えを
  返していた。→ 解決を `iso_resolve_checked` に寄せ、解決の後に世代を比べて変わっていれば捨てて 1 回だけ
  引き直す (2 回とも変われば VFS_ERR_IO)。list_dir はセクタを読んだ後と cb の後にも開始時の世代と比べ、
  変わっていたら VFS_ERR_IO で中断する (cb に渡した分は取り消せないので読み直さない)
- 模型の媒体 B は SUB を 24 に移し、B の 21, 22 には 2 セクタ目が空の囮の SUB を置いた
- 変異 34〜37 (上の 4 つを 1 つずつ戻す) は全部 RED

## 3-3. NP21/W で 1 セクタも読めなかった件 (2026-09-26、fe8305d)

- 報告: `dd cd0 lba=16 count=1` → read error、`mount /cd0 cd0 iso9660` → -1、`cd0: block 1 sects`。ISO は NP21/W の ide3
- NP21/W の実装 (`np21w-src/src/cbus/ideio.c` / `atapicmd.c`) を読んで写した**2 台の模型** (`g_np2`、セカンダリの
  マスター / スレーブ) を足した。同期の経路 (`atapi_dataread`: セクタごとに DRQ 2048、最後のワードで次のセクタか
  完了、エラーレジスタを読むと CHK が落ちる、リセット後の CylLo/Hi = EB14h、READ CAPACITY は**総数**を返し空のドライブは 0) と、
  CD_ASYNC の「読みの後しばらく BSY、DRQ は次のフレーム」を写した
- 分かったこと (机上 + 模型): 新しい手順 (複数セクタ・400ns・受け取った長さの確認) は NP21/W の同期・CD_ASYNC の
  どちらの経路でも読める (`np2_read`、`np2_async`)。**報告の形 (容量 1・READ(10) が失敗) は「選んだ装置が空の CD」**
  のときにだけ出る: fe8305d の atapi.c をこの模型の「マスターに空の CD、スレーブ (ide3) に ISO」で回すと
  `cap total=1`・`read16 rc=-3`、「マスター無し・スレーブに ISO」では `atapi_init` が CD 無しを返す。
  **275df91 以前の atapi.c もマスターしか見ない**ので、同じ構成なら同じに落ちる (以前読めたときは ISO がマスター
  = ide2 だったと考えられる。**NP21/W の ini は読んでいない**ので構成は PM が確かめる)
- 直し: マスターとスレーブの両方を見て、2 台なら媒体の入っている方を選ぶ。セカンダリへ切り替えるたびに選んだ
  装置を選び直す (直す途中、スレーブを見た後の最初の BSY 待ちが居ないスレーブ = 0xFF を見て期限切れになるのを
  模型が見つけた)。READ CAPACITY の長さの確認と、読みの失敗の診断の行を足した
- 試験: `np2_read` / `np2_async` / `np2_empty` (空のドライブで容量 1・sense=5 の行・入れば読める) / `np2_slave`
  (4 通りの構成で選ぶ装置と読み) / `cap_nodata`。変異 38〜43 は全部 RED

## 4. 未検証

- **実機・NP21/W での速さは測っていない** (PM が測る)。NP21/W は CD のシーク・回転を模擬しないので
  速さの差は実機でしか出ない
- 実機の CD ドライブが 16 セクタの READ(10) と 0x8000 の byte count limit を受けるか。困ったら
  `ATAPI_READ_MAX_SECTORS` を下げる
- 実機のドライブが READ(10) で UNIT ATTENTION を返すか (返さなければ 2 秒規則だけが効く)
- HDD (ext2) への書き込みの速さは見ていない。CD 側が速くなった後は、そちらが律速になりうる
