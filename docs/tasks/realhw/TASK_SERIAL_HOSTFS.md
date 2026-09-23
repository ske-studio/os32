# TASK_SERIAL_HOSTFS — 実機でシリアル越しに `/host` をマウントし、`hsync` でカーネルとユーザーランドを更新する

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23) / 状態: **方針レビュー (Codex + Opus サブエージェント、ラリー 1)**。
> ユーザー決裁 (2026-09-23): 「SQLite の分離と LZ4 高圧縮の両方採用」「昔のシリアル経由のホストドライブ (SerialFS) を復活させる方向で」。
> 前提の票: [TASK_HDD_INSTALL.md](TASK_HDD_INSTALL.md) (HDD に入れた後の更新手段)、[TASK_SERIAL_VFAST.md](TASK_SERIAL_VFAST.md) (115200)、CI の配備ツリー (docs/08_build.md §8-6)。

## 0. 事実 (PM が確認)

| # | 事実 | 根拠 |
|---|---|---|
| S1 | 実機でファイルを入れる手段は**無い**。`recv host:` / `push` は `/host` (= NP21/W の HostDrv) に読み替えるだけ、`hotdeploy` のバッファは NP21/W のデバッグ API がメモリへ書く、`hsync` は `/host` が前提 | userland/shell/rshell.c:525〜545, 467〜520、userland/system/hsync.c:7〜17 |
| S2 | **SerialFS** が 2026-04 まであった: VFS ドライバ `fs/serialfs.c` (534 行、`CONFIG_SERIALFS`)、RPC (ENQ + `SF` + cmd、CRC16、LZSS)、コマンド OPEN/READ/WRITE/CLOSE/LS/MKDIR/RMDIR/UNLINK/RENAME/GETSIZE/READ_STREAM/WRITE_STREAM、ホスト側は `tools/os32_server.py` の RPC ハンドラ。**8896d38 (2026-04-23) で「HostDrvFS が完全に代替」として削除**。`git show 8896d38^:fs/serialfs.c` で読める | git |
| S3 | 旧 SerialFS の時間切れは**ループ回数** (`timeout_ms * 10000`、「エミュレータの高速実行対策」)。実機で動いたことは無い。mtime を返す操作が無い (hsync はサイズと日時で差分を決める、stat の mtime が要る) | 8896d38^:fs/serialfs.c:23〜37、fs/vfs.h:116〜122 |
| S4 | 削除の後に、同じシリアル線へ **rshell** (1 行送信 → 応答 + EOT、`rshell_active` 中は console 出力をシリアルへ複写 kernel/console.c:247/283/358) と **V-FAST 115200 の切替 + 番犬** (`serial ack`) が入った。rshell の中から `hsync` を走らせると、RPC のフレームと console の複写が同じ線に混ざる | kernel/console.c、userland/shell/rshell.c |
| S5 | `vmkernel.lz4` = VK32 ヘッダ + 区間 (kernel.bin → 0x100000、sqlite.bin → 0x300000 ※) を**高速モードの LZ4** で詰めた 1 ファイル、507,949B。HDD ローダの上限 `MAX_IMAGE_SIZE` 508KiB (残り約 12KB)。FD ローダは FAT12 の `VMKRNL.LZ4` を 0x10000 から読む | tools/mkvmkernel.py、boot/boot_defs.h:45〜49、boot/boot_main.c:68〜90、boot/loader_fat_new.asm:100〜281 |
| S6 | 実測 (PM、2026-09-23): kernel.bin 281,856 → LZ4 高速 約 201KB / **高圧縮 171,894**、sqlite.bin 374,840 → 高速 約 306KB / **高圧縮 257,980**。展開側 (`lib/lz4.c`) はどちらのモードでも同じ | python lz4.block |
| S7 | 115200 の出力は 3.9 KB/s (ゲストの出力経路が上限)。ゲストの**受信**速度は未測定 | TASK_SERIAL_VFAST |

※ sqlite の配置番地は mkvmkernel の引数に従う (memmap の正典は docs/02_memory.md)。

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

## 2. レビュアーに見てほしいこと

- Q1: 部品 A の 2 ファイル化で、FD ローダ (実モード、FAT12、0x10000 からの連続読み) と HDD ローダ (ext2_mini) の読み込み域・上限・順序に破綻は無いか。1 ファイルのまま区間ごとに読む案と比べてどちらが安全か。
- Q2: SerialFS を rshell と同じ線で多重化する案 (§1-6) に到達可能な混線は無いか (console の複写を止める範囲、割り込み文脈の kprintf、番犬、EOT、ホストの振り分け)。rshell を抜けてから SerialFS を使う (排他) 案と比べてどちらが安全か。
- Q3: VFS の契約 (fs/vfs.h の VfsOps: stat / set_mtime / read_stream / write_stream / list_dir / rename) のうち、hsync が実際に使うものを SerialFS がすべて満たすか。満たせないものは何か。
- Q4: 時間切れ・再送・フレーム長の決め方。実機 115200 の 16 バイト FIFO で取りこぼさない条件。
- Q5: `/boot/*.old` と FD での復旧で、更新失敗から確実に戻れるか。

## 3. 受入 (案)

| ID | 内容 | 場 |
|---|---|---|
| T1 | ホスト試験: フレームの組み立て・CRC32・番号・再送・フレーム長の境界、ホストの振り分け (EOT とフレームの混在)、パスの `..` 拒否 | ホスト |
| T2 | NP21/W: 2 ファイル化したカーネルで FD 起動・NHD 起動・kselftest。HostDrv を切った状態 (ini は変えない — HostDrv の検出を無効にする試験フラグで) で SerialFS を NP21/W のシリアル (名前付きパイプ or TCP) 越しに mount → `ls /host` → `hsync --verify` | NP21/W |
| T3 | 実機: 115200 で `mount /host serial serialfs` → `hsync` でユーザーランド 1 本と `/boot/vmkernel.lz4` を更新 → 再起動 → `ver` の Build が変わる | 実機 (HDD インストール後) |
| T4 | 実機: 壊れたカーネルを入れて起動失敗 → FD 起動 → `.old` で復旧 | 実機 |

## 4. しないこと

ローダ・IPL の更新 (生セクタ)、自動マウント、SerialFS の圧縮、複数クライアント。
