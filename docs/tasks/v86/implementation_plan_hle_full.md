# V86 HLE NP21/W完全準拠化 — 実装計画

[v86_hle_full_comparison.md](file:///mnt/c/WATCOM/src/os32/docs/tasks/v86/v86_hle_full_comparison.md) のギャップ分析に基づく段階的実装計画。

## 調査結果サマリ

V86サブシステム全9モジュール (PIC/PIT/DMA/FDC/INT18h/INT1Bh/GPハンドラ/iocore/BDA) をNP21/Wと比較した結果、**ブートクリティカルな欠落が3件** (P0) 特定された。

> [!IMPORTANT]
> 最も影響が大きいのは **DMAポートの未保護** (P0-1)。BIOS ROMが初期化時にDMA全チャネルを操作するが、現在フォールスルーで実HWに到達しOS32のDMA設定を破壊する危険がある。

---

## Phase 1: DMA全チャネルポート仮想化 (P0-1)

**優先度**: 最高 — BIOS ROM初期化がDMAを直接操作しOS32を破壊する

#### [MODIFY] [v86_dma.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_dma.c)

ch0-3の全レジスタを仮想化する構造体拡張:

```c
/* 4チャネル対応構造体 (ch2のみ転送実行、他は仮想のみ) */
static struct {
    u8  flipflop;
    struct {
        u16 addr;
        u16 count;
        u8  bank;
        u8  mode;
    } ch[4];
    u8  mask;       /* 全4ch分マスクビット */
    u8  status;     /* ステータスレジスタ */
} vdma;
```

追加ポートハンドラ:
- `0x01/0x03` (ch0 addr/count), `0x05/0x07` (ch1 addr/count), `0x0D/0x0F` (ch3 addr/count)
- `0x11` (ソフトウェアリクエスト) — NOP
- `0x1B` (マスタクリア) — 全チャネルリセット
- `0x1D` (全マスクリセット) — mask=0
- `0x1F` (全マスク書込み) — mask=val

推定: ~80行追加

#### [MODIFY] [v86_iocore.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_iocore.c)

新ポートをバインド:
```c
/* DMA ch0: 0x01(addr), 0x03(count) */
v86_io_inp[0x01] = dma_inp; v86_io_out[0x01] = dma_out;
v86_io_inp[0x03] = dma_inp; v86_io_out[0x03] = dma_out;
/* DMA ch1: 0x05(addr), 0x07(count) */
v86_io_inp[0x05] = dma_inp; v86_io_out[0x05] = dma_out;
v86_io_inp[0x07] = dma_inp; v86_io_out[0x07] = dma_out;
/* DMA ch3: 0x0D(addr), 0x0F(count) */
v86_io_inp[0x0D] = dma_inp; v86_io_out[0x0D] = dma_out;
v86_io_inp[0x0F] = dma_inp; v86_io_out[0x0F] = dma_out;
/* DMA制御: 0x11,0x13,0x1B,0x1D,0x1F */
v86_io_inp[0x11] = dma_inp; v86_io_out[0x11] = dma_out;
v86_io_inp[0x13] = dma_inp; v86_io_out[0x13] = dma_out;
v86_io_out[0x1B] = dma_out;
v86_io_out[0x1D] = dma_out;
v86_io_inp[0x1F] = dma_inp; v86_io_out[0x1F] = dma_out;
/* DMAバンク: 0x21(ch0),0x25(ch1),0x27(ch3) */
v86_io_inp[0x21] = dma_inp; v86_io_out[0x21] = dma_out;
v86_io_inp[0x25] = dma_inp; v86_io_out[0x25] = dma_out;
v86_io_inp[0x27] = dma_inp; v86_io_out[0x27] = dma_out;
```

---

## Phase 2: FDD切替レジスタ 0xBE 書込み (P0-2)

**優先度**: 最高 — IO.SYSが2HD/2DD切替時にOUT 0xBEを実行

#### [MODIFY] [v86_fdc.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_fdc.c)

```c
/* 0xBE: FDD切替レジスタ (chgreg)
 * NP21/W io/fdc.c: fdc.chgreg = val
 * bit0: 0=2DD, 1=2HD
 * bit1-7: ドライブ選択・モーター制御 */
case 0xBE:
    if (is_write) {
        fdc_chgreg = *val;  /* 仮想レジスタに保存、実HWには触れない */
    } else {
        *val = fdc_chgreg;
    }
    return 1;
```

推定: ~15行追加

---

## Phase 3: PIC OCW3 SMM (P0-3)

**優先度**: 高 — IO.SYS初期化で特殊マスクモードを使う可能性

#### [MODIFY] [v86_pic.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_pic.c)

```c
/* vpic構造体に ocw3 フィールドを追加 */
u8 ocw3;   /* OCW3: bit5=SMM, bit6=ESMM, bit1=RR, bit0=RIS */

/* OCW3処理を拡張 */
if ((val & 0x18) == 0x08) {
    u8 ocw3 = vpic[idx].ocw3;
    if (!(val & 0x02)) {      /* RR=0: RISビットは変更しない */
        val = (val & ~0x01) | (ocw3 & 0x01);
    }
    if (!(val & 0x40)) {      /* ESMM=0: SMMビットは変更しない */
        val = (val & ~0x20) | (ocw3 & 0x20);
    }
    vpic[idx].ocw3 = val;
    return;
}
```

推定: ~15行追加

---

## Phase 4: DMAバンク全ch + PICローテーション (P1-1, P1-2)

Phase 1でバンクレジスタは同時に実装済み。PICローテーションを追加:

#### [MODIFY] [v86_pic.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_pic.c)

```c
/* OCW2でRビットが立っている場合、pryをローテーション */
if (val & PIC_OCW2_R) {
    vpic[idx].pry = (level + 1) & 7;
}
```

推定: ~15行追加

---

## Phase 5: INT 08h HLE改善 (P1-3)

#### [MODIFY] [v86.c](file:///mnt/c/WATCOM/src/os32/kernel/v86.c)

HLT時のINT 08h消費ロジックを拡充:
- BDAタイマー更新に加え、モータタイムアウト (0040:0040) のデクリメント
- 0のときモーターOFF (BDA DISK_INT更新)

推定: ~25行追加

---

## 修正しないもの (理由)

| 項目 | 理由 |
|------|------|
| PIT クロック精度 | tick(10ms)推定で十分。ゲーム精度は将来課題 |
| DMA TC | HLE FDCがTC不要 |
| FDC FORMAT TRACK | DOSブートで未使用 |
| FDC SCAN系 | DOSブートで未使用 |
| INT 18h AH=0Ah GDC完全 | デフォルト値で動作 |
| PIT BCD | DOSがBCD使用しない |

---

## 検証計画

### ビルド & デプロイ

```bash
make -C /mnt/c/WATCOM/src/os32 kernel
```

`/build-os32` ワークフロー → NP21/W再起動 → hsync

### テスト手順

```bash
# 1. V86セッション起動
curl -sX POST http://localhost:8032/cmd -d "v86 /dos5_1.fdi"

# 2. スクリーンショットでブート観察
curl -sX GET http://localhost:8032/screenshot > screenshot.png

# 3. F12でセッション終了 → v86_diag.log自動生成
curl -sX POST http://localhost:8032/key -d "F12"

# 4. ログ確認
cat /mnt/c/os32/debug/v86_diag.log
```

### 確認ポイント

1. **v86_diag.log [I/O PORT ACCESS LOG]**: 未仮想化ポートへのフォールスルーが減少していること
2. **v86_diag.log [FDC]**: chgregの状態が正しいこと
3. **v86_diag.log [PIC]**: OCW3/SMMの状態
4. **v86_diag.log [DISK I/O LOG]**: INT 1Bh呼出シーケンスの正常進行
5. **スクリーンショット**: FreeDOSブートのIPL→IO.SYS→コマンドプロンプト到達

### 各Phase完了後の回帰チェック

- `make kernel` 成功
- 既存V86テスト (`v86_test.c`) パス
- OS32カーネル `ver` 応答正常
