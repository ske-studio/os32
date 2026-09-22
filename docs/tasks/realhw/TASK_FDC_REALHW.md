# TASK_FDC_REALHW — 実機で FD から起動できない (root panic) を直す

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-22) / 状態: **実装済み・エミュレータ回帰 (2HD) 合格・Codex 往復 2 で Approve**。実機は未検証 (R6)

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
