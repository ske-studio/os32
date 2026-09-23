# TASK_SERIAL_HOSTFS — 実機でシリアル越しに `/host` をマウントし、`hsync` でカーネルとユーザーランドを更新する

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23) / 状態: **v2 — ラリー 1 (Codex / Opus とも Request changes) を反映、ラリー 2 待ち**。
> **ユーザー決裁の変更 (2026-09-23)**: ラリー 1 で「SQLite はカーネルと同じ ELF にリンクされ、カーネル関数を 400 か所以上直接呼ぶ = 分離しても独立更新できない」と判明 → **SQLite の分離は取り下げ、1 ファイルのまま LZ4 高圧縮だけ採用** (選択肢 1)。
> ユーザー決裁 (2026-09-23): 「SQLite の分離と LZ4 高圧縮の両方採用」「昔のシリアル経由のホストドライブ (SerialFS) を復活させる方向で」。
> 前提の票: [TASK_HDD_INSTALL.md](TASK_HDD_INSTALL.md) (HDD に入れた後の更新手段)、[TASK_SERIAL_VFAST.md](TASK_SERIAL_VFAST.md) (115200)、CI の配備ツリー (docs/08_build.md §8-6)。

## 0. 事実 (PM が確認)

| # | 事実 | 根拠 |
|---|---|---|
| S1 | 実機でファイルを入れる手段は**無い**。`recv host:` / `push` は `/host` (= NP21/W の HostDrv) に読み替えるだけ、`hotdeploy` のバッファは NP21/W のデバッグ API がメモリへ書く、`hsync` は `/host` が前提 | userland/shell/rshell.c:525〜545, 467〜520、userland/system/hsync.c:7〜17 |
| S2 | **SerialFS** が 2026-04 まであった: VFS ドライバ `fs/serialfs.c` (534 行、`CONFIG_SERIALFS`)、RPC (ENQ + `SF` + cmd、CRC16、LZSS)、コマンド OPEN/READ/WRITE/CLOSE/LS/MKDIR/RMDIR/UNLINK/RENAME/GETSIZE/READ_STREAM/WRITE_STREAM、ホスト側は `tools/os32_server.py` の RPC ハンドラ。**8896d38 (2026-04-23) で「HostDrvFS が完全に代替」として削除**。`git show 8896d38^:fs/serialfs.c` で読める | git |
| S3 | 旧 SerialFS の時間切れは**ループ回数** (`timeout_ms * 10000`、「エミュレータの高速実行対策」)。実機で動いたことは無い。mtime を返す操作が無い (hsync はサイズと日時で差分を決める、stat の mtime が要る) | 8896d38^:fs/serialfs.c:23〜37、fs/vfs.h:116〜122 |
| S4 | 削除の後に、同じシリアル線へ **rshell** (1 行送信 → 応答 + EOT、`rshell_active` 中は console 出力をシリアルへ複写 kernel/console.c:247/283/358) と **V-FAST 115200 の切替 + 番犬** (`serial ack`) が入った。rshell の中から `hsync` を走らせると、RPC のフレームと console の複写が同じ線に混ざる | kernel/console.c、userland/shell/rshell.c |
| S5 | `vmkernel.lz4` = VK32 ヘッダ + 区間 (kernel.bin → 0x100000、sqlite.bin → **0x200000**) を**高速モードの LZ4** で詰めた 1 ファイル、507,949B。HDD ローダの上限 `MAX_IMAGE_SIZE` 508KiB (残り約 12KB)。FD ローダは FAT12 の `VMKRNL.LZ4` を 0x10000 から読む | tools/mkvmkernel.py、boot/boot_defs.h:45〜49、boot/boot_main.c:68〜90、boot/loader_fat_new.asm:100〜281 |
| S6 | 実測 (PM、2026-09-23): kernel.bin 281,856 → LZ4 高速 約 201KB / **高圧縮 171,894**、sqlite.bin 374,840 → 高速 約 306KB / **高圧縮 257,980**。展開側 (`lib/lz4.c`) はどちらのモードでも同じ | python lz4.block |
| S7 | 115200 の出力は 3.9 KB/s (ゲストの出力経路が上限)。ゲストの**受信**速度は未測定 | TASK_SERIAL_VFAST |


## 1. 方針 (レビュー対象)

### 部品 A — カーネル一式を小さくする (ユーザー採用 2 点)

1. `tools/mkvmkernel.py` を **LZ4 高圧縮** (`mode='high_compression'`, level は実測で決める) にする。展開側は無変更。
2. **SQLite を別ファイルに分ける**: `/boot/vmkernel.lz4` (kernel だけ) と `/boot/sqlite.lz4` (sqlite だけ、同じ VK32 形式)。HDD ローダ (`boot_main.c`) と FD ローダ (`loader_fat_new.asm`、FAT 名 `SQLITE.LZ4`) が 2 つを読んで展開する。**読み込み域** (0x10000〜) を 2 ファイルでどう使うか (順に読んで展開、または別の番地) を決め、`MAX_IMAGE_SIZE` の検査をファイルごとに持つ。片方が無い・壊れているときは画面に出して止まる。インストーラ (cdinst/install)・`nhd_deploy.py`・`deploy.yaml`・FD イメージ (`build/image.mk`)・CI の成果物に `sqlite.lz4` を足す。
3. 見込み: 普段のカーネル更新は約 170KB。

### 部品 B — シリアル越しの `/host` (SerialFS の復活)

4. **VFS ドライバ** `fs/serialfs.c` を旧版から起こし直す。`/host` の既定は従来どおり HostDrv (NP21/W で `hostdrvfs_detect()` が真)。**HostDrv が無いとき**だけ、`mount /host serial serialfs` (明示のコマンド) で SerialFS を `/host` に付ける。自動マウントはしない (シリアルの相手が居るとは限らない)。
5. **プロトコル**: 旧 RPC を土台に、(a) **時間切れは tick** (TASK_HAL_WIRING の時計、`sys_time_now` か tick)、(b) **stat (サイズ + mtime、UNIX 秒 UTC)** と **LS に mtime を載せる**、(c) CRC は **CRC32** (hsync と同じ) に上げる、(d) 1 要求 = 1 応答、要求ごとに通し番号、応答の番号違い・CRC 違いは再送 1 回 → 失敗で `OS32_ERR_IO`、(e) 圧縮 (LZSS) は**付けない** (vmkernel は圧縮済み、実測で要否を決める)、(f) 1 フレームの最大長を決める (ゲストのバッファと 16 バイト FIFO の取りこぼしを考える)。
6. **シリアル線の共有 (rshell との関係)**: SerialFS の要求中は、ゲストの console → シリアルの複写を**止める** (フレームの途中に文字が混ざらない)。ホスト側は 1 本のプログラム (`tools/rshell_serial.py` を拡張、または `tools/serialfs_host.py` を新設して rshell_serial から使う) が、rshell の応答 (EOT 区切りの文字) と SerialFS のフレーム (ENQ で始まり長さを持つ) を**同じポートで振り分ける**。rshell の EOT 約束と `serial ack` の番犬は変えない。
7. **ホスト側サーバ**: ノートで CI の artifact (`hostdrv.tar.gz`) を展開したディレクトリを `/host` として出す。書き込み (push、hsync のマニフェスト) は許すがルートの外は断る (`..` と絶対パス)。
8. **使い方**: ノートで `rshell_serial.py --fast 115200 --serve-host <dir> repl` → ゲストで `mount /host serial serialfs` → `hsync` (必要なら `hsync sys`、`hsync boot`) → 再起動。
9. **安全**: `/boot/vmkernel.lz4` と `/boot/sqlite.lz4` は hsync の一時名 → 検証 → rename (既存、§4-36) で置き換わる。さらに**置き換える前に旧版を `/boot/*.old` に残す** (hsync の `/boot` 専用の前処理)。起動しなくなったら FD で起動して `/hd0/boot/*.old` を戻す (手順を文書に)。ローダ・IPL の生セクタは hsync では**書かない**。

## 1-v2. ラリー 1 による改訂 — §1 の該当項をこの節で置き換える

**部品 A (1 ファイル + 高圧縮 + 壊れの検出)**
- A-1 はそのまま (mkvmkernel を高圧縮、level は 9 と 12 を実測)。**A-2 (SQLite の分離) は取り下げ**。見込み 約 430KB、上限 508KiB まで約 90KB。
- **A-4 壊れの検出** (両者): VK32 に **CRC32 表を追加**する。既存のエントリ (16B × n) の後ろに `entry_count` 個の CRC32 (展開後のデータに対して) と、ファイル全体の CRC32 を置き、`header_size` を増やす。**旧ローダとの互換**: 旧ローダがエントリを `16 + i*16` の固定位置で読み、`data_offset` を明示で使うなら、表の追加は旧ローダに見えない (実装者がコードで確認して報告。互換にならないなら止めて報告)。新ローダ (HDD `boot_main.c` + `lz4_mini.c`、FD `loader_fat_new.asm` の ASM デコーダ) は: 完全長 (ext2 の切り詰めは段 0 で止まる形に済)、`entry_count ≥ 1`、各エントリの `data_offset + comp_size ≤ ファイル長`、展開先が許可範囲 (0x100000〜の帯) に収まる、**`decoded == raw_size`**、CRC32 一致、を検査し、外れたら画面に出して止まる。FD の ASM デコーダが CRC32 を持つのが重ければ、FD は長さと範囲の検査だけにし CRC は HDD ローダだけ (FD は媒体ごと書き直す運用なので) — 実装者の判断を報告。

**部品 B (SerialFS)**
- **B-4' マウント**: `vfs_dev_parse` が受ける名前に合わせて **`mount /host COM1 serialfs`**。`/host` が既にマウントされていれば断る (vfs_mount が同じ prefix の二重登録を断るよう直す)。`rshell.c` の `serial N` に残っている `sys_mount("/host", "COM1", "serialfs")` (旧版の名残) は**削除**。自動マウントはしない。
- **B-6' 線の共有 = 遠隔から入れる排他モード** (Codex の「排他」と Opus の「遠隔操作が要る」の両立): SerialFS の要求は **SerialFS セッションの中でだけ**出る。セッションはマウント時に始まりアンマウント (または rshell の `sfs end`、時間切れの連続) で終わる。セッション中は、(a) ゲストの**送信の所有者ゲート** (`serial_putchar` の手前、console の複写・KAPI の serial_putchar/serial_puts・`ime_dict` の serial_puts・`isr_handlers` の polled 出力をすべて通す) が SerialFS 以外の送信を**捨てる**、(b) rshell・`kbd.c` の 4 か所 (rshell_active 中にシリアルを読む) はシリアルを読まない、(c) ホスト側は線を**占有**して SerialFS の要求に答え続け、利用者の入力は送らない。セッションの終わりに rshell は EOT を 1 つ返し、ホストは rshell の会話に戻る。**使い方**: ホストで `rshell_serial.py --fast 115200 --serve-host <dir> cmd "mount /host COM1 serialfs && hsync boot && umount /host"` のように 1 行で投げ、ホストはその 1 行の完了 (セッション終了後の EOT) まで線を占有する。ゲートは ring3_fault_kill / exec_exit / rshell のループの全経路で下ろす。IF=0 のまま待たない (tick が進まない)。
- **B-5' プロトコル** (両者):
  - フレーム = `ENQ 'S' 'F'` + 種別 + 通し番号 (u16) + 長さ (u16) + ペイロード + **CRC32 (種別〜ペイロード全体)**。ペイロード上限 **512B** (応答が大きい READ / LS は分割し「続き」フラグと offset/cookie で継続)。
  - 受信は 1 つの受信器がフレーム全体 (長さ 0・エラー応答も) を消費して検証する (旧版の残留バイト問題を持ち込まない)。長さは確保・コピーの前に上限と照合。
  - **番号違い・CRC 違いのフレームは数えずに捨て**、期限まで待ち続ける。時間切れ (tick: 最初の 1 バイト 2s、バイト間 100ms、全体 = 長さ × 87µs × 2 + 余裕) なら同じ番号で**再送** (最大 3 回)、それでも駄目なら `OS32_ERR_IO`。時間切れの後は「線が 200ms 静かになるまで読み捨て」てから戻る。**連続 N 回 (既定 3) 失敗したら線を死んだと見なし**、アンマウントまで即座に IO を返す。
  - **ホストは (セッション, 番号) ごとに最後の応答を保存**し、同じ番号の再送には**操作を再実行せず保存した応答を返す** (RENAME / UNLINK / MKDIR / WRITE の二重実行を防ぐ)。
  - 圧縮 (LZSS) は付けない。
- **B-3' VFS 契約** (Opus Q3): `stat` (st_mode の種別、st_size、0 でない st_mtime)、`get_file_size` (ディレクトリは ISDIR)、`list_dir` (ファイルには NOTDIR、エントリの境界検査)、`read_stream` (要求より多く返さない、EOF 前に 0 を返さない)、`write_stream` / `write_file` (push 用)。「無い」は NOTFOUND、通信失敗は IO に分ける。LS に mtime は載せない (hsync は別に stat する)。set_mtime / create_excl は NOSYS でよい。
- **B-7' ホスト側サーバ**: 受信を常時読むスレッド、フレームには常に答える、コマンドの完了はフレームの外の EOT、時間切れは「進捗が無い時間」で数える、配信中は reset_input_buffer を使わない。ルートの封じ込めは **realpath** (symlink 経由の脱出を断る)、wire 上のパスは `/` 始まりの「ルート相対」でホストの絶対パスと区別。書き込みは push のパスだけ許す (hsync はマニフェストを**読むだけ**)。
- **B-9' `.old`** (両者): `/boot/vmkernel.lz4` を置き換える前に、**それが今動いているビルドと同じとき (`ver` の Build と VK32 のビルド時刻が一致) だけ** `/boot/vmkernel.old` を作る (一度も起動していない版で上書きしない)。作り方は複製 → 検証 → rename (hsync の既存の一時名の経路)、本名が存在しない時間を作らない。**戻す範囲**: カーネルだけ。同じ実行で `hsync sys` によって KAPI が上がっていたら、shell.bin 等も戻す必要がある旨を文書に。ローダの自動切り替えはこの票ではしない (FD 起動 → `/hd0/boot/vmkernel.old` を戻す手順を文書に)。ext2 は非ジャーナルなので、電源断での媒体破損までは保証しない。
- **性能**: CI のビルドは毎回全ファイルの mtime を変えるので、`hsync` の既定 (全体) はシリアル越しでほぼ全ファイルを読み比べる (数分〜十数分)。**当面は `hsync boot` / `hsync bin` と範囲を絞って使う**旨を文書に。ホストが CRC を返す操作は別票。
- **ISR の計数**: `drivers/serial.c` の ISR は OE / FE を数えずにリセットしている → 数えて `serial` コマンドに出す (実機の切り分け用)。

## 2. レビュアーに見てほしいこと

- Q1 (v2 で取り下げ、参考): 部品 A の 2 ファイル化で、FD ローダ (実モード、FAT12、0x10000 からの連続読み) と HDD ローダ (ext2_mini) の読み込み域・上限・順序に破綻は無いか。1 ファイルのまま区間ごとに読む案と比べてどちらが安全か。
- Q2: SerialFS を rshell と同じ線で多重化する案 (§1-6) に到達可能な混線は無いか (console の複写を止める範囲、割り込み文脈の kprintf、番犬、EOT、ホストの振り分け)。rshell を抜けてから SerialFS を使う (排他) 案と比べてどちらが安全か。
- Q3: VFS の契約 (fs/vfs.h の VfsOps: stat / set_mtime / read_stream / write_stream / list_dir / rename) のうち、hsync が実際に使うものを SerialFS がすべて満たすか。満たせないものは何か。
- Q4: 時間切れ・再送・フレーム長の決め方。実機 115200 の 16 バイト FIFO で取りこぼさない条件。
- Q5: `/boot/*.old` と FD での復旧で、更新失敗から確実に戻れるか。

## 3. 受入 (案)

| ID | 内容 | 場 |
|---|---|---|
| T1 | ホスト試験: フレームの組み立て・CRC32・番号・再送・フレーム長の境界、ホストの振り分け (EOT とフレームの混在)、パスの `..` 拒否 | ホスト |
| T2 | NP21/W: 高圧縮 + CRC 表の vmkernel で FD 起動・NHD 起動・kselftest、**旧ローダでも新 vmkernel が起動する** (互換)、CRC を壊した vmkernel で両ローダが止まる。HostDrv を切った状態 (ini は変えない — HostDrv の検出を無効にする試験フラグで) で SerialFS を NP21/W のシリアル (名前付きパイプ or TCP) 越しに mount → `ls /host` → `hsync --verify` | NP21/W |
| T3 | 実機: 115200 で `mount /host serial serialfs` → `hsync` でユーザーランド 1 本と `/boot/vmkernel.lz4` を更新 → 再起動 → `ver` の Build が変わる | 実機 (HDD インストール後) |
| T4 | 実機: 壊れたカーネルを入れて起動失敗 → FD 起動 → `.old` で復旧 | 実機 |
| T5 | ホスト + NP21/W: 障害注入 — 応答の喪失・遅延 (番号ずれ)・CRC 破損・ホストの停止 (線が死ぬ判定)・再送で副作用が二重にならない・セッション中に rshell へフレームが漏れない・`cat` した本文に ENQ/EOT があってもセッション外で混線しない | ホスト / NP21/W |

## 4. しないこと

ローダ・IPL の更新 (生セクタ)、自動マウント、SerialFS の圧縮、複数クライアント。
