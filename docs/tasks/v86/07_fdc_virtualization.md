# 7. FDC/DMAポートレベル仮想化

ソース: [`v86_fdc.c`](../../../kernel/v86_fdc.c), [`v86_dma.c`](../../../kernel/v86_dma.c)

## 7.1 背景

INT 1Bh BIOS経由ではなく、ゲストがFDCポート (0x90/0x92/0x94) に直接I/Oする
ケースが存在する:

- **FORMAT.COM**: フォーマットコマンドはFDCに直接コマンドを発行
- **コピープロテクション**: 特殊セクタIDの検出にREAD IDを直接実行
- **ディスクコピーソフト**: セクタ単位の生READ/WRITE

## 7.2 FDCステートマシン

µPD765A互換FDCは4フェーズで動作する:

```
  IDLE ──[コマンドバイト]──▶ COMMAND ──[最終パラメータ]──▶ EXECUTE
   ▲                                                        │
   │                                                        │
   └──────────────── RESULT ◀───────────────────────────────┘
                   [最終リザルト読み出し]
```

### MSR (Main Status Register) — 0x90 Read

| フェーズ | MSR値 | 意味 |
|---------|-------|------|
| IDLE | 0x80 (RQM) | コマンド受付準備完了 |
| COMMAND | 0x90 (RQM\|BUSY) | パラメータ受信中 |
| EXECUTE | 0x10 (BUSY) | DMA転送中 (実際は瞬時完了) |
| RESULT | 0xD0 (RQM\|DIO\|BUSY) | リザルト読み出し準備完了 |

### FIFO (データレジスタ) — 0x92 Read/Write

- **Write** (IDLE/RESULT → COMMAND): 新コマンドバイト受信
- **Write** (COMMAND): パラメータバイト受信。全パラメータ受信後 → EXECUTE → fdc_execute_command()
- **Read** (RESULT): リザルトバッファから1バイト返却。全リザルト返却後 → IDLE

## 7.3 対応FDCコマンド詳細

### SPECIFY (0x03) — 2パラメータ, 0リザルト

SRT/HLT/HUT を仮想レジスタに記録するだけ。仮想化環境では実効なし。

### SENSE DRIVE STATUS (0x04) — 1パラメータ, 1リザルト

```
パラメータ: [1] HD/US
リザルト:   [0] ST3
  ST3 bit5 (RY) = 1 (常にReady)
  ST3 bit4 (T0) = 1 if PCN=0
  ST3 bit0-2 = HD/US
```

### READ DATA (0x06) — 8パラメータ, 7リザルト

```
パラメータ:
  [1] HD/US  [2] C  [3] H  [4] R  [5] N  [6] EOT  [7] GPL  [8] DTL

処理:
  1. v86_dma_get_transfer() でDMAバッファアドレス+サイズ取得
  2. 実FDD → fdc_read_sector_geom()
     loop_dev → loop_dev_read_chs()
  3. リザルト: ST0/ST1/ST2/C/H/R/N

リザルト (成功時):
  [0] ST0 (0x00)  [1] ST1 (0x00)  [2] ST2 (0x00)
  [3] C  [4] H  [5] R+1  [6] N
```

### WRITE DATA (0x05) — READ DATAと対称

DMAバッファからディスクイメージへの書き込み。

### RECALIBRATE (0x07) — 1パラメータ, 0リザルト

仮想PCN=0にリセット。`irq_after_seek = 1` でSENSE INTERRUPT待ち。

### SENSE INTERRUPT STATUS (0x08) — 0パラメータ, 2リザルト

RECALIBRATE/SEEK後のステータス確認。

```
リザルト:
  [0] ST0 (0x20 = Seek End)
  [1] PCN (現在シリンダ)
```

`irq_after_seek` がセットされていない場合: ST0=0x80 (Invalid Command)。

### READ ID (0x0A) — 1パラメータ, 7リザルト

現在トラック (vfdc.pcn) のセクタ1のIDを返す。
ジオメトリから sec_n を取得。

### FORMAT TRACK (0x0D) — 5パラメータ, 7リザルト

```
パラメータ:
  [1] HD/US  [2] N  [3] SC  [4] GPL  [5] D (フィルパターン)

処理:
  1. DMAバッファから CHRN × SC のIDフィールド配列を取得
  2. 各セクタをフィルパターンDで埋めて書き込み
  3. 1セクタずつ loop_dev_write_chs / fdc_write_sector_geom
```

### SEEK (0x0F) — 2パラメータ, 0リザルト

仮想PCNを更新。`irq_after_seek = 1`。

### CTRLレジスタ (0x94 Write)

モーター/リセット/DMA有効ビットを仮想レジスタに保存。

リセット検出: bit7が 1→0 に遷移した場合、FDCステートマシンを初期化。
ST0=0xC0 (リセット後ステータス) に設定し、SENSE INTERRUPTを4回要求。

### リードスイッチ (0x94 Read)

`0x04` (2HD両面ドライブ) を返す。

## 7.4 DMA連携

FDCのREAD/WRITEコマンドはDMA ch2経由でデータ転送する。
仮想DMAレジスタから転送パラメータを計算:

```c
u8 *v86_dma_get_transfer(u32 *out_bytes) {
    *out_bytes = (u32)vdma.count + 1;  /* DMAはcount-1を格納 */
    linear = ((u32)vdma.bank << 16) | vdma.addr;
    linear &= 0xFFFFF;  /* 1MB境界でクリップ */
    seg = (u16)(linear >> 4);
    off = (u16)(linear & 0x0F);
    return v86_phys_addr(seg, off);  /* バッキングRAM内のポインタ */
}
```

### フリップフロップ

DMAのアドレス/カウントレジスタは8bitポートで16bit値を設定する。
Low→High の順序で書き込まれ、フリップフロップで切り替え:

```
OUT 0x19, xx  → フリップフロップ = 0 (Low)
OUT 0x09, Lo  → addr = (addr & 0xFF00) | Lo, FF = 1
OUT 0x09, Hi  → addr = (addr & 0x00FF) | (Hi<<8), FF = 0
```

## 7.5 エラーコード (ST1/ST2)

| ST1値 | 意味 | 発生条件 |
|-------|------|---------|
| 0x04 | No Data | セクタ長コード不一致, CHS範囲外 |
| 0x20 | CRC Error | FDC読み書き失敗 (実FDDモード) |
| 0x50 | Overrun | DMAバッファ未設定 |
| 0xC0 | Equipment Check | loop_devスロット未設定 |
| 0xE0 | No Loop Slot | loop_dev未アタッチ |

ST0 bit6-7:
- 0x00: 正常終了
- 0x40: 異常終了 (IC=01)
- 0x80: Invalid Command
- 0xC0: リセット後 (Polling Sense)
