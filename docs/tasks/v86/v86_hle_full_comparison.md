# V86 HLE 全体比較: OS32 vs NP21/W

NP21/W のI/Oポートレベル (io/) およびBIOSレベル (bios/) の実装と
OS32 V86サブシステムの各モジュールを比較した網羅的なギャップ分析。

生成日: 2026-05-16

---

## 1. PIC (8259A) — v86_pic.c vs io/pic.c

### OS32 (300行)
- ICW1-4シーケンス: IMR/ISR/IRRクリアのみ。ICW値は保存しない。
- OCW2 (EOI): 非特殊EOI (0x20) と特殊EOI (0x60+n) をサポート。
- OCW3: read_isr フラグでISR/IRR切替のみ。SMM (Special Mask Mode) 未対応。
- 優先度ローテーション: 未対応 (pry固定=0)。
- IRQ注入: v86.c/v86_vsync.c から直接ISR/IRR操作。

### NP21/W (452行)
- ICW1-4: icw[4]配列に全ICW値を保存。ICW3でカスケード、ICW4で動作モードを設定。
- OCW2: R(ローテーション)+SL(指定レベル)+EOI の全組合せ対応。ローテーション (pry) あり。
- OCW3: RIS/RR (レジスタ読出し選択) + SMM (Special Mask Mode) + ESMM をocw3に保存。
- pic_irq(): 優先度ローテーション(pry)に基づくIRQ配信。マスタ→スレーブカスケード完全実装。
- nevent_forceexit(): EOI/IMR変更時にCPUループから即座に抜ける。

### ギャップと影響

| 項目 | NP21/W | OS32 | 影響度 | 説明 |
|------|--------|------|--------|------|
| OCW2 R (ローテーション) | ✅ | ❌ | 中 | DOSはpry=0固定で使用するため影響小。ゲームで問題の可能性 |
| OCW3 SMM (特殊マスクモード) | ✅ | ❌ | **高** | IO.SYS/FreeDOS初期化でSMMを使う可能性あり |
| ICW値保存 | ✅ (icw[4]) | ❌ (破棄) | 低 | ICW2のベクタベースはV86固定のため影響小 |
| nevent_forceexit | ✅ | ❌ | 中 | OS32はシングルタスクのため不要だが、EOI後の即時IRQ配信に影響 |
| pic_irq 優先度処理 | ✅ (pry) | ❌ (固定) | 中 | DOSは優先度変更しないため影響小 |

---

## 2. PIT (8253A) — v86_pit.c vs io/pit.c

### OS32 (251行)
- 3カウンタ: mode/rw_mode/reload_value/write_phase/read_phase/latch を管理。
- Counter#0: tick_countベースのカウンタ推定。リロード値保存。
- Counter#1 (ビープ): コマンド/データともハードウェアにパススルー。
- Counter#2 (RS-232C): RS-232C保護で blocked。
- ラッチ/リードバック: ラッチ対応、リードバック(SC=11)は無視。
- IRQ分周比: reload / DEFAULT_RELOAD でIRQ0注入タイミングを調整。

### NP21/W (547行)
- 3カウンタ + beep/ppi連携。
- Counter#0: クロック精度のカウンタシミュレーション (nevent_setでタイマイベント)。
- ゲートピン制御: mode/gate/countの精密なシミュレーション。
- OUT F3h (8251リセット): SYSTEMタイマリセット時の特殊処理。
- BCD/バイナリモード: bit0でBCD/バイナリ選択。

### ギャップと影響

| 項目 | NP21/W | OS32 | 影響度 | 説明 |
|------|--------|------|--------|------|
| カウンタ精度 | クロック精度 | tick(10ms)推定 | 中 | DOSブートには影響小。ゲームの精密タイマで問題 |
| ゲートピン制御 | ✅ | ❌ | 低 | DOSはゲートピン操作しない |
| BCD モード | ✅ | ❌ | 低 | BIOSは通常バイナリモード使用 |
| リードバック | ✅ | ❌ (無視) | 低 | DOSブートで使用されない |
| beep連携 | ✅ | パススルー | OK | OS32はパススルーで正常動作 |

---

## 3. DMA (µPD8237A) — v86_dma.c vs io/dmac.c

### OS32 (200行)
- ch2のみ: addr/count/bank/mode/mask/flipflop を管理。
- 1チャネル固定: ch0/ch1/ch3は未サポート。
- 転送: v86_dma_get_transfer() でFDC HLEから直接呼出し (実DMAは不使用)。

### NP21/W (446行)
- 4チャネル全て: ch0(NONE), ch1(2DD-FDC), ch2(2HD-FDC), ch3(SASI/SCSI)。
- DMA転送実行: dmac_getdatas/dmac_senddata でメモリ↔デバイスの実転送。
- TC (Terminal Count): DMA完了通知をデバイスに送信。
- dmac_check(): mask/ready/workの3段階管理で動的にチャネル活性化。
- ch3バウンドモード: 64KB境界をまたぐ転送。
- バンク上位バイト (0xE05): 25bitアドレッシング (IA32時)。

### ギャップと影響

| 項目 | NP21/W | OS32 | 影響度 | 説明 |
|------|--------|------|--------|------|
| ch0/ch1/ch3 | ✅ | ❌ | 中 | FDC HLEがch2を直接使用するため実害は小。SCSIは未対応 |
| TC (Terminal Count) | ✅ | ❌ | 低 | HLE FDCがTC不要のため |
| dmac_check() 動的管理 | ✅ | ❌ | 低 | HLE方式では不要 |
| バンク25bitアドレス | ✅ | ❌ | 低 | V86は1MB以内 |
| 0x01/0x03ポート (ch0 addr/count) | ✅ | ❌ | **高** | BIOS ROMが初期化時にch0を設定する可能性あり |
| 0x11/0x1B/0x1D/0x1Fポート | ✅ | ❌ | **高** | DMAマスタリセット/オールマスク操作がトラップされない |

---

## 4. FDC (µPD765A) — v86_fdc.c vs io/fdc.c

### OS32 (823行)
- ステートマシン: IDLE/CMD/EXEC/RESULT の4フェーズ。
- MSR (0x90): ステートに応じたRQM/DIO/CB/D0B/NDMビット生成。
- FIFOリード (0x92): コマンドフェーズでの書込み、リザルトフェーズでの読出し。
- SENSE INT STATUS (08h): ST0 + PCN 応答。
- READ DATA/WRITE DATA/READ ID: HLE方式 (loop_dev経由)。
- SPECIFY (03h): ack応答のみ。
- sync_rw/sync_seek: HLE操作後のFDCステート同期。
- モーター制御 (0x94): motor_on/off 状態管理 + 物理I/Oブロック。

### NP21/W (1174行)
- 上記全て + 以下:
- FDD切替レジスタ (0xBE): chgreg による2HD/2DD/外付け切替。
- SCAN EQUAL/SCAN LOW-OR-EQUAL/SCAN HIGH-OR-EQUAL: コンペア系コマンド。
- FORMAT TRACK (0Dh): トラックフォーマット。
- エラーシミュレーション: CRCエラー、セクタ未検出、オーバーランの精密再現。
- fdc_interrupt(): neventベースのIRQ遅延発行。
- DMA連携: dma_dmaready/fdc_dmafunc/TC処理。
- fdd_int(): FDC I/O操作後のメインループ待機（結果ステータス応答の同期）。
- R/W バッファ管理: buf[0x4000] でセクタデータをバッファリング。

### ギャップと影響

| 項目 | NP21/W | OS32 | 影響度 | 説明 |
|------|--------|------|--------|------|
| chgreg (0xBE) 2HD/2DD切替 | ✅ | 部分的 (読取のみ) | **高** | IO.SYSがINP 0xBEでFDDタイプを判定する |
| FORMAT TRACK (0Dh) | ✅ | ❌ | 低 | DOSブートでは不使用 |
| SCAN系コマンド | ✅ | ❌ | 低 | DOSブートでは不使用 |
| エラーシミュレーション | ✅ | ❌ | 中 | IO.SYSのリトライロジックに影響する可能性 |
| IRQ遅延発行 | ✅ (nevent) | ❌ (即時同期) | 中 | HLE方式では即時同期で十分 |
| 0xBE OUT (chgreg書込み) | ✅ | ❌ | **高** | IO.SYSが密度切替時に書き込む |

---

## 5. INT 18h (CRT BIOS) — v86_bios.c vs bios/bios18.c

### OS32 (560行, INT 18h部分 ~300行)
- KBD: AH=00h-05h (入力/状態/シフト/初期化/keystate) 実装済み。
- CRT: AH=0Ah-0Fh (モード設定/取得/表示ON-OFF/カーソル/文字RW) 実装済み。
- AH=16h: VRAMクリア (DL/DH対応済み)。
- AH=17h: アトリビュート設定 (実装済み)。
- AH=1A/1Bh: スクロールアップ/ダウン (up実装, down NOP)。
- AH=40h-43h: グラフィック表示ON-OFF + パレット (ack/パススルー)。

### NP21/W bios18.c (1336行)
- 上記全て + 以下:
- AH=0Ah: GDCパラメータ完全設定 (SYNC/ラスタ/PRAM/アクティブ行数)。
- AH=09h: テキスト1文字書き込み (BDA カーソル管理込み)。
- AH=1Ah: ユーザ文字定義 (CG RAM書込み)。
- AH=40h-49h: グラフィックBIOS完全 (パレット/カラーコード/解像度/GDC初期化)。
- CRTCレジスタ操作: GDC SYNC/PRAM コマンドシーケンス。

### ギャップと影響

| 項目 | NP21/W | OS32 | 影響度 | 説明 |
|------|--------|------|--------|------|
| AH=0Ah GDCパラメータ | ✅ (完全) | ❌ (ack) | 中 | DOS標準モードではデフォルト値で動作するため影響小 |
| AH=09h テキスト書込み | ✅ | ❌ (ROM委譲) | 低 | ROM実行で代替可能 |
| AH=1Bh スクロールダウン | ✅ | ❌ (NOP) | 低 | DOSブートで未使用 |
| AH=40h-49h グラフィック | ✅ (完全) | 部分的 | 中 | ゲーム用。DOSブートには不影響 |

---

## 6. INT 1Bh (ディスクBIOS) — v86_disk.c vs bios/bios1b.c

### OS32 (1227行)
- AH=00h (SEEK), 01h/06h (READ), 03h (INITIALIZE+equip), 04h/84h (SENSE)。
- AH=05h/15h (WRITE/FORMAT WRITE), 02h (診断読み), 07h (RECALIBRATE)。
- AH=0Ah (READ ID), 0Eh (密度設定)。
- FDCステートマシン同期 (sync_rw/sync_seek)。

### NP21/W bios1b.c (1336行)
- 上記全て + 以下:
- AH=0Dh (FORMAT TRACK): CHS + セクタ長パラメータ付きフォーマット。
- fddbios_equip(): 完全なBDA DISK_EQUIP計算ロジック。
- SEEK/READ/WRITE: FDC I/O操作経由 (fdc_out/fdc_in) + DMA連携。
- リトライロジック: エラー時の自動リトライ + ステータス返却。
- 2HD/2DD切替: F2HD_MODE/F2DD_MODE によるドライブタイプ自動判定。

### ギャップと影響

| 項目 | NP21/W | OS32 | 影響度 | 説明 |
|------|--------|------|--------|------|
| FORMAT TRACK (0Dh) | ✅ | ❌ | 低 | DOSブートで未使用 |
| リトライロジック | ✅ | ❌ | 低 | HLEは1発成功が前提 |
| fddbios_equip完全版 | ✅ | 部分的 | 低 | 基本的なビット設定は実装済み |

---

## 7. GPハンドラ (v86.c) — INT分岐 + IRQ注入

### OS32 (1781行)
- INT: 0x1B→HLE, 0x29→HLE, 0x18/1C/11/12→ROM委譲, 0x20/21→終了。
- IRQ注入: IRQ0 (タイマ), IRQ1 (KBD), IRQ2 (VSYNC) をサポート。
- IF/CLI/STI: v86_virtual_if ソフトウェアフラグで管理。
- HLT: 保留IRQ消費 (BDAタイマ直接更新) + NOP続行。
- I/O: 8bit/16bit INP/OUT → v86_iocore ディスパッチ。
- PUSHF/POPF/IRET: IF/VM ビット操作のエミュレーション。
- FAR CALL (9Ah): ROM FAR CALLのサポート。
- REP INS/OUTS: 完全対応。

### 既知の問題/欠落

| 項目 | 状態 | 影響度 | 説明 |
|------|------|--------|------|
| INT 08h ROM HLE | 部分的 | **高** | ROM INT 08hハンドラへの注入がトリプルフォルト。HLT時のBDA更新のみ |
| INT 1Ch フック注入 | 部分的 | 中 | FreeDOSのInit_clk_driverが設定するINT 1Chフック |
| LOCK PREFIX | NOP | OK | |
| FWAIT | NOP | OK | |
| セグメントオーバーライド | 読飛し | OK | |
| 0x66 (OPSZ) | 部分的 | 低 | 32bit I/O操作は未サポート (16bit V86で不要) |

---

## 8. I/Oディスパッチ (v86_iocore.c) — ポートカバレッジ

### 登録済み仮想化ポート

| ポート | デバイス | 方式 |
|--------|---------|------|
| 0x00,0x02 | PIC マスタ | 仮想 |
| 0x08,0x0A | PIC スレーブ | 仮想 |
| 0x09,0x0B,0x15,0x17,0x19,0x23 | DMA ch2 | 仮想 |
| 0x30,0x32,0x33,0x35,0x75,0x77 | RS-232C | ブロック |
| 0x41,0x43 | KBD 8251A | 仮想 |
| 0x60,0xA0 | GDCステータス (VSYNC) | 仮想INP |
| 0x64 | VSYNCトリガ | 仮想 |
| 0x71,0x73 | PIT Counter#0,#1 | 仮想 |
| 0x90-0xBE | FDC | 仮想 |
| 0xEC,0xEE | HostDrv | ブロック |
| 0xF0 | リセット | ブロック |
| 0xF2,0xF6 | A20ゲート | 仮想 |

### 未登録 (実HWフォールスルー) で問題のあるポート

| ポート | デバイス | NP21/W | 影響度 | 説明 |
|--------|---------|--------|--------|------|
| 0x01,0x03,0x05,0x07 | DMA ch0/ch1 addr/count | dmac_o01/o03 | **高** | BIOS ROMが初期化時に全チャネルを設定 |
| 0x11 | DMA ソフトウェアリクエスト | dmac_o13 | 中 | |
| 0x1B | DMAマスタクリア | dmac_o1b | **高** | BIOS初期化で使用 |
| 0x1D | DMAオールマスクリセット | dmac_o1d | **高** | BIOS初期化で全チャネルアンマスク |
| 0x1F | DMAマスクレジスタ(全ch) | dmac_o1f | **高** | BIOS初期化で使用 |
| 0x21,0x23,0x25,0x27 | DMA ch0-3 バンク | dmac_o21 | 中 | ch0/ch1/ch3のバンク設定 |
| 0x37 | ブザー/システムポート | bios系 | 低 | ブザーON/OFF。フォールスルーで動作 |
| 0x5F | WAITポート | 直接OUT | OK | I/Oウェイト用。フォールスルーで問題なし |
| 0x68 | GDCモードフリップフロップ | io/gdc.c | 低 | テキスト/グラフィックモード切替 |
| 0x6A | パレットGDC | io/gdc.c | 低 | |
| 0xBE | FDD切替レジスタ | io/fdc.c | **高** | 2HD/2DD切替。現在読取のみ、書込み未対応 |

---

## 9. メモリ / BDA — v86_mem.c

### 実装済み (NP21/W準拠)
- BDA全フィールド: SYS_TYPE, BIOS_FLAG0/1/3/5, CRT系, ディスク系, KBD, メモリ, GRCG, メモリスイッチ全て。
- F2HD_MODE/F2DD_MODE/F2DD_POINTER/F2HD_POINTER。
- IVT: BIOS ROMベクタテーブルからINT 00h-1Fhを設定。
- INT 1Bh: HLEスタブ配置。
- ページテーブル: TVRAM/GVRAM/ROM/A20ラップアラウンド全対応。

### ギャップなし ✅

---

## 優先度別 実装タスク一覧

### P0: ブートクリティカル (即時対応)

| # | タスク | ファイル | 推定行数 | 説明 |
|---|--------|---------|----------|------|
| P0-1 | DMA全チャネルポート仮想化 | v86_dma.c + v86_iocore.c | ~80行 | 0x01,0x03,0x05,0x07 (ch0-3 addr/count)、0x1B (マスタクリア)、0x1D (全マスクリセット)、0x1F (全マスク) をトラップ。BIOS ROM初期化が直接DMAハードウェアに触れるとOS32側DMAが破壊される |
| P0-2 | FDD切替レジスタ0xBE書込み | v86_fdc.c | ~15行 | OUT 0xBE のchgreg書込みを仮想レジスタに保存。IO.SYSが2HD/2DD切替時に使用 |
| P0-3 | PIC OCW3 SMM (特殊マスクモード) | v86_pic.c | ~10行 | ocw3にSMM/ESMMビットを保存し、pic_irq相当でSMM考慮。IO.SYSが使う可能性 |

### P1: ブート安定化

| # | タスク | ファイル | 推定行数 | 説明 |
|---|--------|---------|----------|------|
| P1-1 | DMAバンクレジスタ全ch | v86_dma.c + v86_iocore.c | ~30行 | 0x21,0x25,0x27 のch0/ch1/ch3バンクレジスタを仮想化 |
| P1-2 | PIC OCW2 ローテーション | v86_pic.c | ~15行 | R+EOI, R+SL+EOIのpry更新を追加 |
| P1-3 | INT 08h注入改善 | v86.c | ~30行 | ROM INT 08hスキップ時のBDAタイマー更新+モータタイムアウト管理の完全化 |

### P2: 機能補完

| # | タスク | ファイル | 推定行数 | 説明 |
|---|--------|---------|----------|------|
| P2-1 | INT 18h AH=1Bh スクロールダウン | v86_bios.c | ~20行 | 現在NOPの部分を実装 |
| P2-2 | FDC FORMAT TRACK (0Dh) | v86_disk.c | ~40行 | ディスクイメージへのフォーマット書込み |
| P2-3 | PIT リードバック | v86_pit.c | ~15行 | SC=11のリードバックコマンド |

---

## 実装計画 (推奨順序)

### Phase 1: DMAポート保護 (P0-1)

BIOS ROMが初期化時に全DMAチャネルを操作する。現在フォールスルーで実HWに到達し、
OS32のDMA設定を破壊する危険がある。

```
修正ファイル: v86_dma.c, v86_iocore.c
追加ポート:
  0x01 (ch0 addr), 0x03 (ch0 count)
  0x05 (ch1 addr), 0x07 (ch1 count)
  0x0D (ch3 addr), 0x0F (ch3 count)  ※PCによっては
  0x11 (ソフトウェアリクエスト)
  0x13 (DMAステータス/コマンド — NP21/Wでは0x13を使用)
  0x1B (マスタクリア)
  0x1D (全マスクリセット)
  0x1F (全マスク書込み)
  0x21,0x25,0x27 (ch0/ch1/ch3 バンク)
方針: 仮想レジスタに保存するのみ。実DMAには一切アクセスしない。
```

### Phase 2: FDD切替レジスタ (P0-2)

```
修正ファイル: v86_fdc.c
ポート 0xBE:
  現在: INP→仮想値0x01返却, OUT→未トラップ(実HW到達)
  修正: OUT時にchgreg仮想レジスタに保存。
        bit0 = 0: 2DD, bit0 = 1: 2HD (デフォルト)
```

### Phase 3: PIC SMM (P0-3)

```
修正ファイル: v86_pic.c
OCW3処理に SMM/ESMM ビットの保存を追加。
SMM有効時、ISRのビットが立っているレベルでもIMRを無視してIRQを配信する動作。
```

### Phase 4: DMAバンク + PICローテーション (P1-1, P1-2)

```
v86_dma.c: ch0/ch1/ch3のバンクレジスタを仮想化。
v86_pic.c: OCW2のR(ローテーション)ビット処理を追加。
```

### Phase 5: INT 08h改善 (P1-3)

```
v86.c: HLT時のINT 08h HLE改善。
  - BDAタイマー更新だけでなく、モータタイムアウト管理も含める。
  - FreeDOS Init_clk_driver のINT 08hフックが設定された後は
    フックハンドラに注入する方式への切替を検討。
```

---

## テスト・検証

1. **ビルド**: `make kernel` でエラーなし
2. **デプロイ**: `/build-os32` ワークフロー
3. **V86テスト**: `v86 /dos5_1.fdi` でブート
4. **F12終了→v86_diag.log**: DMA/PIC/FDC ログセクションを確認
5. **スクリーンショット**: ブート進行度の目視確認
6. **v86_diag.log I/Oログ**: 未仮想化ポートへのフォールスルーアクセスがないか確認
