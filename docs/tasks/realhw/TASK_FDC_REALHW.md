# TASK_FDC_REALHW — 実機で FD から起動できない (root panic) を直す

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-22) / 状態: **実機で合格 (R6、2026-09-22、bee42cc)** — FD 起動 → シェル、シリアル経由で `ver` / `ls` が返る

基点: `feat/gui` `ec48c6b`。実機計画は [`PLAN.md`](PLAN.md)、1.44MB の経緯は [`TASK_FD144.md`](TASK_FD144.md)、
FDC ドライバの仕様表は [`../../05_drivers.md`](../../05_drivers.md) §5-2。

## 0. 症状 (ユーザー報告、2026-09-22)

実機 PC-9821Ra266 で FD から起動すると、カーネルの `MOUNT...` の右に赤い **`root panic`**。
**1.2MB 2HD (`os32_boot.d88` 由来) と 1.44MB (`os32_boot144.img`) の両方**で同じ。
HDD は接続されているが未フォーマット (または他 OS の内容)。

ユーザーの当初の見立ては「新規インストール時は ext2 が無いので当たり前」だったが、
**FD 起動の経路に ext2 の依存は無い** (§1)。`root panic` は **fd0 の FAT マウント失敗**である。

## 1. 机上で潰したこと

| 疑い | 結論 | 根拠 |
|---|---|---|
| ext2 が無いと起動できない | **否** | `kernel/kernel.c` は `boot_drive` が FDD なら `fd0`+`fat` をルートにし、`/hd0` は ext2 → iso9660 → fat と試して**全部失敗しても続行**する。ext2 は superblock の magic 不一致で `EXT2_ERR_MAGIC` を返すだけ |
| 空 / 他 OS の HDD でマウント試行がハングする | **否** | `ide_wait_*` は全部ループ上限つき。`ext2_find_partition` は読めなければ既定 LBA に落ちる。`iso9660_mount` は CD 以外を断る。`pc98_find_fat_partition` は空エントリで抜ける |
| HostDrv が無いと止まる | **否** | `hostdrvfs_detect()` が 0 を返せばマウントしない (TASK_FD144 F11 で実測) |
| IPL / ローダ | **否** | どちらも BIOS INT 1Bh で読む。カーネルまで到達している (`MOUNT...` が出る) |
| **カーネル自前の FDC ドライバ** | **ここ** | fd0 の FAT マウントは `fs/fatfs/diskio.c` → `fdc_read_sector()` (`drivers/fdc.c`) で、BIOS を使わない。**実機で初めて走った** |

## 2. 原因

`drivers/fdc.h` の **`FDC_IRQ_TIMEOUT_TICKS = 20` (200ms)** が NP21/W に合わせた値で、実機の機構に足りない。

| 動作 | 実機の所要時間 | 200ms との関係 |
|---|---|---|
| RECALIBRATE / SEEK | SPECIFY が SRT=8ms なので **最大 77〜80 トラック × 8ms ≒ 620〜640ms**。ローダが `VMKRNL.LZ4` を読んだ直後のヘッドはシリンダ 20〜40 付近 → 160〜320ms | **`fdc_init()` の recalibrate がタイムアウト** |
| READ DATA | 1 回転 (360rpm=167ms / 300rpm=200ms) + ヘッドロード 10ms + 転送 16ms | ぎりぎり。運次第 |

**NP21/W はシーク時間を模擬していない**: `np21w-src/src/io/fdc.c` の `fdc_intwait` は
`nevent_set(NEVENT_FDCINT, 512, ...)` — 512 サイクル後に割り込みが来る。だからエミュレータでは
200ms で一度も困らなかった (POLICY_DEBUG §4-49 のシリアルと同じ型: **模擬していない量は「確認済み」にならない**)。

**タイムアウトの後に回収が無いので事故が連鎖する**:
遅れて来た seek-end 割り込みを SENSE INTERRUPT STATUS で読み出さないまま次の SEEK を出すと、
µPD765 の INT 線が上がりっぱなしになり、PIC (エッジトリガ) に次のエッジが来ない。以後の
`fdc_wait_irq` が**全部**タイムアウトし、3 回リトライしても読めず `RES_ERROR` → `f_mount` 失敗 → `root panic`。
1.2MB / 1.44MB の両方で同じになるのは、この経路が媒体に依らないから。

## 3. 直し方 (コーダーへの依頼、§4 に結果)

1. タイムアウトを機構に合わせる: シーク/リキャリブレート 1.5s、R/W 1s、リセット 0.5s (根拠は `fdc.h` のコメント)。
2. シーク完了待ちを堅牢に: IRQ を待ち、タイムアウトしても SIS で ST0 を読み、SE が立っていれば「エッジ取りこぼし」として完了扱い。
3. SEEK / RECALIBRATE の前に SIS で未回収割り込みを**排水** (上限 4 回、0x80 で止める)。
4. リトライの間に `fdc_recover()` (リセット → Specify → 排水 → recalibrate)。
5. RECALIBRATE の EC (77 ステップで届かない) はもう 1 回。
6. DMA バッファを `aligned(1024)` にして [HW2] を**リンク順に頼らず**保証。起動時に検査。
7. 最終失敗のときだけ `[fdc] ... st0/st1/st2` を 1 行出す ([V4]、実機の画面で読める)。

## 4. 結果 (コーダー Opus 5、`fix/fdc-realhw`)

| 項目 | 実装 |
|---|---|
| 時間上限 | `FDC_SEEK_TIMEOUT_TICKS` 150 / `FDC_RW_TIMEOUT_TICKS` 100 / `FDC_RESET_TIMEOUT_TICKS` 50。旧 `FDC_IRQ_TIMEOUT_TICKS` は R/W の別名として残置 |
| 判定の切り出し | `drivers/fdc_decide.[ch]` — `fdc_sis_result_bytes()` (SIS の 1 バイト応答) と `fdc_classify_seek_end()` (OK / RETRY_EC / PENDING / FAIL)。I/O も tick も触らない |
| 排水 | `fdc_drain_interrupts()` を SEEK / RECALIBRATE / リセット後に。ST0=80h で打ち切り、上限 4 |
| 完了待ち | `fdc_wait_seek_end()` — IRQ を待ち、タイムアウトしても SIS。別ドライブの通知は読み捨て |
| 回復 | `fdc_recover()` を R/W リトライの**間**にだけ |
| EC | `FDC_RECAL_ATTEMPTS` = 2 |
| DMA | `aligned(1024)`、番地は 0x1555e0 → 0x156000。起動時検査 |
| 診断 | `[fdc] read/write fail ... phase= st0= st1= st2=`、`[fdc] recalibrate drv= rc= st0=` |
| ホスト TDD | `tools/tests/test_fdc_seek.py --target --mutate` (6 ケース、変異 4 本が全部 RED)。記録 `tools/tests/fdc_seek_tdd.md`。`make check-par` に `check-fdc-seek-host` |

**コーダーが見つけて設計に入れたこと**: pending 無しの SIS は ST0=80h の **1 バイトだけ**返る (NP21/W `FDC_SenceintStatus` も同じ)。
旧コードは無条件に 2 バイト読んでいたので、排水のたびに来ない 2 バイト目を 10000 回空転して待つところだった。

**失敗経路の所要時間**: 全部タイムアウトする最悪ケースで 1 セクタ約 11.5 秒 (旧 1.2 秒)。媒体無しのドライブは
NR 付きの割り込みが即座に来る (実機の µPD765A も NP21/W の `FDC_Seek`/`FDC_Recalibrate` も) ので、普段は踏まない。

**変えていないこと**: SRT 8ms、モーター制御、`fdc_motor_off()` (未使用のまま)、既存 API のシグネチャ、`kernel/kernel.c`。

## 5. 検証の段取り

| ID | 見るもの | 手段 |
|---|---|---|
| R1 | ホスト TDD (判定関数) | コーダー |
| R2 | `make kernel` / `make all` / `make check` | テスター |
| R3 | **2HD FD 起動の回帰** (NP21/W): `root OK`、シェル、`/bin/cfg.bin` md5 一致 | テスター/PM |
| R4 | **1.44MB FD 起動の回帰** (NP21/W): 同上 | テスター/PM |
| R5 | HDD 起動の回帰 (fd0 は /fd0 にサブマウント) | テスター |
| **R6** | **実機で FD 起動 → `root OK` → シェル** | **ユーザー**。失敗時は `[fdc]` の行を写真で |

**エミュレータでは原因そのものは再現できない** (§2)。R3〜R5 は退行が無いことしか言わない。

### 5-1. 結果 (2026-09-22)

| ID | 結果 |
|---|---|
| R1 | **合格**。6 ケース、変異 4 本が全部 RED (`make check-fdc-seek-host`) |
| R2 | `make all` / `make fd144` / **`make check` (exit 0) 合格** (b299ea9、本体で実行。worktree は `.env` が無く `/usr/local/cross` 既定になるので全体ビルドには使えない) |
| R3 | **合格**。NP21/W を trial ini + `os32_boot.d88` 引数で起動 → `[fatfs] mounted: type=1 drv=0 pdrv=0 FAT12`、kselftest 85/85、`OS32 v1.0 (FDD Boot)`、`ver` の Build が新ビルド。`/bin/cfg.bin` (38,272B) がホスト原本と md5 一致、`/VMKRNL.LZ4` (484,794B、474 クラスタ) が**イメージ内のファイルと md5 一致** (原本との 2 バイト差はビルド時刻の秒。イメージ生成後に再リンクされたため)。`[fdc]` の失敗行は出ない。**往復 2 (b299ea9) でも再確認**: リセット後の画面に `[fdc] dma>1MB: 0439h ff -> ff` (新コードの印。NP21/W は 0439h を読めず FFh) → `[fatfs] mounted` → 85/85 → FDD Boot、`/VMKRNL.LZ4` (485,330B) がイメージ内容と md5 一致 |
| R4 | **未実施**。trial ツールが `.d88` しか引数に取れず、1.44MB は生 `.img`。手で挿入するか、ツールの拡張が要る |
| R5 | 未実施 (往復 2 の後、NHD 配備が要る → [D1]) |
| R6 | **未実施** (ユーザー) |

## 6. 独立レビュー (Codex `codex exec -s read-only`)

**往復 1 (b95479d)**: Request changes。PM が反例の到達可能性を確かめて全部採用:

| # | 所見 | 判断 |
|---|---|---|
| B1 (P1) | 最終 READ がタイムアウトしたまま戻ると DMA ch2 が動いたままで、次の WRITE が `dma_buffer` へ写した内容を遅れた旧転送が上書きし得る | 到達可能。最終失敗時に DMA マスク + FDC リセット (`fdc_abort_transfer`) |
| B2 (P2) | `fdc_wait_seek_end` の IRQ 待ちが 1 回だけなので、別ドライブの通知 (drv1 の Ready 変化) で自ドライブの正常なシークを PENDING で打ち切る | 稀だが到達可能。期限までループ |
| B3 (P2) | 媒体無しの `/fd0` 試行で NR 即失敗のたびに reset (NP21/W はリセット IRQ を出さない → 0.5s × 2) | 到達可能。NR は再試行しない + リセット待ち 100ms |
| 非 blocker | `fdc_read_results` の `i--` が無限になり得る / 診断の ST0 が SEEK のものでない / NP21/W の時間説明 (`FDC_INT_DELAY`=6 の後に 512 サイクル) | 全部直す |
| **非 blocker → 実機の第 2 原因** | **`I/O 0439h bit2` = 1MB 以上への DMA 禁止、ノーマルモードの起動時設定は 1** (`io_dma.md` 428 行〜)。`dma_buffer` は 0x156000 で 1MB 超。NP21/W は `necio_o0439` が値を保存するだけで DMA が見ない (`src/io/necio.c`) ので**エミュレータでは再現しない** | **blocker 4 として採用**: `fdc_init` で RMW して bit2 を落とす (bit7 はプリンタ I/F 選択なので他ビットを保つ)。**FFh でも書く** — 実機は bit7=1・bit2=1・未使用ビット 1 で FFh を返し得るので避けると直らない。読み戻しを `[fdc] dma>1MB: 0439h xx -> xx` で報告 (NP21/W は in ハンドラが無く `ff -> ff`) |

**往復 2 (b299ea9)**: **Approve**。往復 1 の blocker 3 件 + 非 blocker 4 件を 1 件ずつ「閉じた」と確認。
到達可能な反例を伴う blocker なし。非 blocker と PM の扱い:

| # | 所見 | 扱い |
|---|---|---|
| 1 | SIS 失敗時に `s_last_seek_st0` が更新されず、診断が前回の値 (例: 古い NR) を出し得る。結果が 1〜2 バイトのときの ST1/ST2 は 0 で埋まる | **残件** (診断の精度。動作には影響しない) |
| 2 | 「NR は再試行しない」が `fdc_init` の 100ms 待ち再 recalibrate と、`fdc_recover` 後の 1 回の SEEK には及んでいない | **残件** (空ドライブで数十 ms の余分。往復 1 の reset ×2 は復活しない) |
| 3 | コメント: 「Specify はリセットで消える」は誤り (SRT/HUT/HLT は RESET で保持)。「20〜40 シリンダなら 200ms で必ず」は強すぎる (20 × 8ms = 160ms) | **直した** (コメントのみ) |
| 4 | ホスト試験は判定関数だけで、待機ループ・abort の I/O 順序・NR 時の reset 回数は検査していない (記録は限界を明記済み) | **残件** (I/O を伴うので実機/エミュレータの領分) |

## 7. 実機での確認 (R6) の手引き

1. `make all` → `images/os32_boot.d88` (1.2MB、`images/os32_boot.img` が生イメージ) / `make fd144` → `images/os32_boot144.img` (1.44MB、生イメージ)。
2. FD から起動し、**画面の次の行を写す**:
   - `[fdc] dma>1MB: 0439h xx -> yy` — xx の bit2 が立っていて yy で落ちていれば 1MB 制限は解けた。`ff -> ff` のように落ちない機種なら別の手 (DMA バッファを 1MB 未満へ) が要る。
   - `MOUNT... root OK` と `[fatfs] mounted: type=1 drv=0 pdrv=0 FAT12`。
   - 失敗時: `[fdc] recalibrate drv=0 rc=.. st0=..` / `[fdc] read fail drv=0 chs=c/h/s phase=<seek|cmd|irq|result> st0=.. st1=.. st2=..` — phase と ST0 で原因が切り分けられる (seek + NR = Ready 不成立、irq = 転送が終わらない、result の ST1/ST2 = CRC/欠落など)。

## 8. 実機 R6 (2026-09-22、b299ea9 相当、1.2MB) — 不合格

写真: `DEV...OK` の右に赤い **`ER`** (= `fdc_init()` の失敗)、`MOUNT... root panic`。**`kprintf` の行が 1 行も無い**。

| 発見 | 内容 |
|---|---|
| ext2 は無関係 (ユーザーの再確認) | `root panic` はルート (fd0) の失敗でだけ出る。`/hd0` の ext2 を試すのは `root OK` の後 |
| **kprintf が実機で見えない** | `kprintf(0x07, …)` などの属性は PC/AT 流で、`tvram_putchar_at` が PC-98 の属性 VRAM へ直書き。PC-98 は bit0 = 表示なので 0x0A/0x0C/0x0E は非表示、0x07 は黒の反転。呼び出しは 0x07 が 73 か所。**エミュレータでは `/api/tvram` が文字コードを返すので誰も気づかなかった**。`/api/screenshot` で裏取り: 同じ瞬間の tvram に `[fdc]`/`[ide]`/`[fatfs]` の行があるのに、画面には 0xC1 (黄) の `[selftest]` と状態行しか映らない — 実機の写真と同じ姿 |
| **FRY (0x94 bit6) を立てていない** | `io_fdd.md` 191 行: RDY はドライブの RDY と FRY の OR。READY 線を出さないドライブでは FRY 無しだと µPD765A が全コマンドを NR で即終了。旧コードは `SE` だけ見て NR の recalibrate を成功扱い → READ で落ちて root panic。新コードは NR を即失敗 → `ER`。**両方の写真と整合**。NP21/W は `ctrlreg & 0x40` を見るが媒体入りは `fdd_diskready` で Ready → 露見しない |

往復 3: kprintf の属性変換 (+ kselftest)、`CTRL_FRY`、状態行 5 に `FDC rc= st0= 0439h=` を `tvram_print` で出す (kprintf に依らない保険)。

### 8-1. 往復 3 の結果 (6ee6fc0 + 状態行の移動)

| 項目 | 内容 |
|---|---|
| kprintf 属性 | `lib/kprintf_attr.c` `kprintf_attr_to_pc98()` (0x07→0xE1、0x0A→0x81、0x0C→0x41、0x0E→0xC1、PC-98 流は素通し)。kselftest 10 件 (85→95)、`check-kprintf-attr-host` (256 通りの不変条件、変異 6 本 RED)。**エミュレータの `/api/screenshot` で `[fdc]` `[ide]` `[fatfs]` が白で見えることを確認** |
| FRY | `CTRL_FRY` (0x40) を 0x94 の動作時の書き込み 3 か所に。NP21/W の副作用: 未搭載ドライブの RECALIBRATE が NR→EC (2 回出して失敗、`fdc_init` は無視)、搭載済み・媒体無しは NR→成功 (`FDC...ER` が `OK` に見える)。SEEK/READ の媒体検査は FRY を見ないので読み書きは従来どおり NR 即断 |
| 状態行 | `FDC rc= st0= 0439h=xx->yy` を**最下行 (行 24)** に `tvram_print` + 同じ内容を `[fdc] …` で kprintf。行 5 は IDE のログに root panic の前に上書きされた (Codex 往復 3 の blocker、エミュレータでも `ff->ff` の尻尾だけ残った) |
| 検証 | `make all` / `make fd144` / `make check` exit 0。NP21/W 2HD FD 起動: 行 24 に `FDC rc=0 st0=20 0439h=ff->ff`、`[fatfs] mounted FAT12`、シェル到達 |

**Codex 往復 3 (6ee6fc0)**: Request changes — blocker は状態行の上書き 1 件 (上のとおり直した)。非 blocker: FRY で実機の空ドライブは NR 早期終了が使えず READ の IRQ タイムアウト (1s × 3) まで待ち得る (有限、DMA マスク/リセットは残る) / `tv_cat` は [C2] の kstring 方針からずれる (境界違反なし) / 属性変換は CGA の背景色・点滅を一般には識別しない (現在の呼び出しに反例なし)。3 往復で Approve に至っていないので、争点 (状態行の置き場) の決着はユーザー判断に委ねる (ROLES §5)。

### 8-2. 実機で見てほしいもの (更新)

- 最下行の `FDC rc=<n> st0=<xx> 0439h=<xx>-><yy>`: rc=0 なら FDC 初期化は通った。st0 の bit3 (0x08) が立っていれば Not Ready (FRY でも直らない = ドライブ選択/モーターの問題)。0439h の yy で bit2 (0x04) が落ちていれば DMA の 1MB 制限は解けた。
- `[fdc] …` の白い行、`MOUNT... root OK`、`[fatfs] mounted`。

## 9. 実機 R6 — 合格 (2026-09-22、bee42cc、1.2MB)

- ユーザー報告: **FD からブートした**。シェルは起動直後に rshell (シリアル待ち) に入るので、
  シリアル未接続だと「ハング」に見える (ESC でローカルへ)。
- USB シリアル (FTDI、Windows の COM3) + クロスケーブルで、`tools/rshell_serial.py` (新規、
  Windows 側 Python + pyserial) から `ver` / `ls /` / `ls /bin` が返った。応答 296 バイト + EOT が 0.8 秒
  (約 490 B/s、9600bps)。Build は `Sep 22 2026 16:19:26` (bee42cc のビルド)。
- 原因は 3 つ重なっていた: §2 (シーク時間のタイムアウト) + §6 (`0439h` bit2 の DMA 1MB 制限) + §8 (FRY)。
  どれもエミュレータでは再現しない。**`0439h` の読み戻しと `FDC rc=` の行 (最下行) の実機の値は未記録** —
  次回の起動で写真を取る。
- 残件: R4 (エミュレータの 1.44MB 回帰)、R5 (HDD 起動の回帰、NHD 配備)、実機の 1.44MB 起動、
  Codex 往復 3 の非 blocker (§8-1)、`tools/rshell_serial.py` をテスターの「叩く側」に組み込む (PLAN §4 段 3)。

### 9-1. 実機で続けて分かったこと (2026-09-22)

| 件 | 内容 |
|---|---|
| シリアル速度 | Ra266 は `[ser] 9600bps (clk 2457600Hz, count 16)` = **2.4576MHz 系**。`serial 38400` (count 4) で切り替えて `ver` が返った。**ただし実効は 9600 で 490 B/s、38400 で 233 B/s** — 回線ではなくゲストの `serial_putchar` (TxRDY を 100 回スピン → `hlt` で次の 10ms tick まで待つ) が上限。速度を活かすには TxRDY のスピンを 1 文字時間ぶん (38400 なら 0.3ms) 取るか、送信割り込みで回す (残件)。19200 (count 8) も整数。57600 / 115200 は分周が割り切れないので `0434h` の 4 分周 (極性未決着、§4-50) が要る |
| **画面の桁ズレ** | 起動完了後の画面が行ごとに 1 文字 (8 ドット) ずつ右へずれる (滑らかな傾き)。root panic で止まった画面 (BIOS の同期のまま) にはずれが無い。OS32 の SYNC パラメータは資料の標準値 (`C/R=4Eh, HS=07, HFP=09, HBP=07, VFP=07, VBP=19h, L/F=190h`) と一致し、NP21/W も同じ値でテキスト幅を出す。**有力な原因**: `gfx/backend_pegc.c` の `pegc_shutdown` → `pegc_gdc_set_timing(400)` が 24kHz を強制 (`PEGC_HSYNC_24KHZ`) してテキスト GDC の SYNC を送り直し、液晶 (LCD172VXM) の自動調整が新しいタイミングに再ロックしていない。CRT では出ない型。**確認はモニタの自動調整を 1 回押す**。直すなら「480 ラインへ入らない起動では BIOS の同期に触らない」 |

## v3 の予定: トラック / シリンダ単位の読み出し (ユーザー決定 2026-09-23)

現行のカーネル FDC ドライバは Read Data の R = EOT = 同じセクタ番号で **1 コマンド 1 セクタ** (2HD で 1KB)。次のコマンドが間に合わないと 1 回転 (360rpm で約 167ms) を待つので、最悪 6KB/s 前後になる (推定、未測定)。
**v3 で 1 トラック (以上) を 1 コマンドで読む**: EOT = トラックの最終セクタ、MT=1 で同じシリンダのヘッド 1 まで続ける (2HD で最大 16KB)。根拠は Bible 2-9 の INT 1Bh データ読み出し (06h): BX = 転送バイト数、読み出しは指定シリンダ内、MT 指定でヘッド 0 → 1。**書き込みは MT 不可** (μPD765 が正しく動かない、同資料) なので 1 トラックずつ。DMA の受け皿は 16KB に広げ、64KB 境界 ([HW2]) と `0439h` (1MB 超の DMA、§4-51) を満たす場所に置く (DMA プール 0x2E8000 の span が候補、TASK_HAL_WIRING)。MS-DOS が BIOS を何バイト単位で呼んでいたかは資料に記述が無く未確認。
着手前に実機で FD 起動の所要時間を測って効果を見積もる。
