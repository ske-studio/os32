# NP21/W vs OS32 BIOS HLE 機能比較

NP21/W `bios/` の各BIOS関数とOS32 `kernel/v86_bios.c`, `v86_disk.c`, `v86_fdc.c` の対応表。
✅=実装済み 🔶=部分的 ❌=未実装 ⬜=不要

---

## INT 1Bh — ディスクBIOS

**NP21/W**: `bios1b.c` (~600行) + `bios.c` (ブート)
**OS32**: `v86_disk.c` (1181行) + `v86_fdc.c` (824行)

### メインディスパッチ `bios0x1b()`

NP21/Wは `CPU_AL & 0xF0` (DA上位ニブル=devtype) でデバイス種別を判別:

| devtype | デバイス | NP21/W処理 | OS32対応 |
|---------|---------|-----------|---------|
| 0x90 | 2HD FDD (type=3,rpm=0) | `fdd_operate(3,0,FALSE)` | ✅ UA=0のみ |
| 0x30,0xB0 | 2HD 1.44MB (type=3,rpm=1) | `fdd_operate(3,1,FALSE)` | ❌ rpm=1未対応 |
| 0x10 | 2DD (type=1) | `fdd_operate(1,0,FALSE)` | ✅ geom切替で対応 |
| 0x70 | 2D (type=0) | `fdd_operate(0,0,FALSE)` | ✅ geom切替で対応 |
| 0xF0 | 2DD alt (type=2) | `fdd_operate(2,0,FALSE)` | ❌ |
| 0x50 | 2D ndensity (type=0,nd=TRUE) | `fdd_operate(0,0,TRUE)` | ❌ |
| 0x00,0x80 | SASI HDD | `sasibios_operate()` | ⬜ 不要 |
| 0x20,0xA0 | SCSI | `scsibios_operate()` | ⬜ 不要 |

**NP21/W戻り値処理** (OS32で重要):
```c
CPU_AH = ret_ah;
flag = MEMR_READ8(CPU_SS, CPU_SP+4) & 0xfe;  /* スタック上のFLAGS */
if (ret_ah >= 0x20) flag += 1;                 /* CF=1 (エラー) */
MEMR_WRITE8(CPU_SS, CPU_SP + 4, flag);
```
→ OS32: `regs[V86_REG_EFLAGS] |= 1` で対応済み ✅

### FDDサブ機能 `fdd_operate()`

| AH&0F | 機能 | NP21/W実装 | OS32 HLE | ギャップ |
|-------|------|-----------|---------|---------|
| **00** | SEEK | `biosfd_seek(CL,nd)` + `fdd_int(SEEKSUCCESS)` | ✅ `fdc_treg`更新 + `sync_seek()` | ✅ OK |
| **01** | VERIFY | SEEK + `fdd_read()`ループ(データ破棄) | ❌ 未実装 | ⚠️ **追加必要** |
| **02** | 診断読み | READ同等 + ARS対策 `b0patch()` | 🔶 ack応答のみ | ⚠️ READにフォールスルーすべき |
| **03** | INITIALIZE | `fddbios_equip(type,FALSE)` → BDA更新 | 🔶 ack応答のみ | ⚠️ **equip更新欠落** |
| **04** | SENSE | WP+密度+ドライブ種別 | ✅ 概ね一致 | ✅ OK |
| **05** | WRITE | `fdd_write()`ループ + DMA境界チェック | ✅ loop_dev経由 | ✅ OK |
| **06** | READ | `fdd_read()`ループ + マルチトラック | ✅ loop_dev経由 | ✅ OK |
| **07** | RECALIBRATE | `biosfd_seek(0,0)` + `SEEKSUCCESS` | ✅ `sync_seek(0)` | ✅ OK |
| **0A** | READ ID | `fdd_readid()` → CL=C,DH=H,DL=R,CH=N | ✅ geomのN値返却 | ✅ OK |
| **0D** | FORMAT | `fdd_formatinit()` + IDフィールド書込 | ✅ v86_fdc.c | ✅ OK |
| **0E** | 密度設定 | `F2HD_MODE`/`F2DD_MODE` BDA更新 | ❌ 未実装 | 🔶 一部DOSで使用 |

### NP21/W `fddbios_equip()` — OS32に欠落

```c
void fddbios_equip(REG8 type, BOOL clear) {
    diskequip = GETBIOSMEM16(MEMW_DISK_EQUIP);  /* BDA 0x055C */
    if (clear) diskequip &= 0x0f00;
    if (type & 1) {  /* 2HD */
        diskequip &= 0xfff0;
        diskequip |= (fdc.equip & 0x0f);
    } else {         /* 2DD */
        diskequip &= 0x0fff;
        diskequip |= (fdc.equip & 0x0f) << 12;
    }
    SETBIOSMEM16(MEMW_DISK_EQUIP, diskequip);
}
```
→ **OS32対応**: `v86_bios_int1b()` case 0x03 で BDA 0x055C を更新する処理を追加

### NP21/W `fdd_int()` — FDCステートマシン同期

```c
static void fdd_int(int result) {
    fdc.stat[fdc.us] = (fdc.hd << 2) | fdc.us;
    switch(result) {
        case FDCBIOS_SUCCESS:     fdcsend_success7(); break;
        case FDCBIOS_SEEKSUCCESS: fdc.stat[us]|=SE; fdc_interrupt(); 
                                  fdc.event=NEUTRAL; fdc.status=RQM; break;
        case FDCBIOS_READERROR:   fdc.stat[us]|=IC0|ND; fdcsend_error7(); break;
        case FDCBIOS_NONREADY:    fdc.stat[us]|=IC0|NR; fdcsend_error7(); break;
        case FDCBIOS_WRITEPROTECT: fdc.stat[us]|=IC0; fdcsend_error7(); break;
    }
}
```
→ **OS32対応**: `v86_fdc_sync_rw()` / `v86_fdc_sync_seek()` で概ね対応済み ✅

### NP21/W `bios0x1b_wait()` — FDD割り込み待ち

```c
UINT bios0x1b_wait(void) {
    if (fdc.chgreg & 1) { addr=DISK_INTL; bit=0x01; }
    else                { addr=DISK_INTH; bit=0x10; }
    bit <<= fdc.us;
    if (mem[addr] & bit) { mem[addr] &= ~bit; return 0; }  /* 完了 */
    else { CPU_IP--; return 1; }  /* 再試行 */
}
```
→ **OS32対応**: HLEでは即完了するため不要。ポートレベル(v86_fdc.c)ではIRQ11+BDA更新で対応済み ✅

---

## INT 12h/13h — FDC割り込みハンドラ

**NP21/W**: `bios12.c` (~80行) — FDC IRQ後のSENSE INTERRUPTシーケンス

```c
void bios0x12(void) {
    iocore_out8(0x08, 0x20);  /* マスタPIC EOI */
    status = iocore_inp8(0xC8);  /* FDC MSR */
    while(1) {
        if (!(status & CB)) {
            iocore_out8(0xCA, 0x08);  /* SENSE INTERRUPT CMD */
            status = iocore_inp8(0xC8);
        }
        result = iocore_inp8(0xCA);  /* ST0読み出し */
        drv = result & 3;
        drvbit = 0x10 << drv;
        /* 結果をBDA 0x05D0 or 0x05D8+drv*2 に格納 */
        mem[MEMB_DISK_INTH] |= drvbit;  /* 完了フラグ */
    }
}
```
→ **OS32**: HLEではBIOS ROMのINT 12hハンドラは実行されない。`v86_fdc_sync_*()` が同等の処理を行う ✅

---

## INT 18h — CRT/キーボードBIOS

**NP21/W**: `bios18.c` (1336行)
**OS32**: `v86_bios.c` (350行)

| AH | 機能 | NP21/W | OS32 | ギャップ |
|----|------|--------|------|---------|
| **00** | キー入力(block) | ✅ BDAバッファ参照 | ✅ `kbd_trygetkey()` | ✅ |
| **01** | キーセンス | ✅ BDAバッファpeek | ✅ `kbd_peekkey()` | ✅ |
| **02** | シフト状態 | ✅ `SHIFT_STS` | ✅ `kbd_shift_state` | ✅ |
| **03** | KBD初期化 | ✅ ポート0x43操作+BDAクリア | ✅ ack | 🔶 BDA初期化欠落 |
| **04** | キー入力状態 | ✅ `KB_KY_STS` | ✅ `kbd_key_pressed[]` | ✅ |
| **05** | キーバッファ読出 | ✅ | ✅ | ✅ |
| **0A** | CRTモード設定 | ✅ **GDC MODE1+ラスタ設定(30行)** | 🔶 ack応答のみ | ⚠️ **GDC操作欠落** |
| **0B** | CRTモード取得 | ✅ `modenum[]`参照 | ✅ 0x00固定 | ✅ |
| **0C** | テキスト表示ON | ✅ GDC START+SYNC | ✅ `outp(0x62,0x0D)` | ✅ |
| **0D** | テキスト表示OFF | ✅ GDC STOP | ✅ `outp(0x62,0x0C)` | ✅ |
| **0E** | ファンクションキー行 | ✅ 表示領域設定 | ✅ ack | ✅ DOS用途では十分 |
| **0F** | カーソル位置取得 | ✅ GDC CSRW読出 | ✅ 変数参照 | ✅ |
| **10** | カーソルタイプ | ✅ `csrform[]` | ✅ ack | ✅ |
| **11** | カーソル表示ON | ✅ GDC CSRFORM操作 | ✅ ack | 🔶 |
| **12** | カーソル表示OFF | ✅ GDC CSRFORM操作 | ✅ ack | 🔶 |
| **13** | カーソル位置設定 | ✅ GDC CSRW | ✅ GDC CSRW | ✅ |
| **14** | フォント読出 | ✅ `bios0x18_14()` | ✅ TVRAM直書 | ✅ (用途が異なる) |
| **15** | ライトペン | ✅ NOP | ⬜ 不要 | ✅ |
| **16** | VRAM初期化 | ✅ `bios0x18_16(DL,DH)` | ✅ `tvram_clear_all()` | 🔶 DL/DH未使用 |
| **17** | ブザーON | ✅ `outp(0x37,0x06)` | ❌ 未実装 | 🔶 |
| **18** | ブザーOFF | ✅ `outp(0x37,0x07)` | ✅ ack (NP21/Wと同名) | 🔶 ポート操作欠落 |
| **19** | ライトペン初期化 | ✅ NOP | ✅ ack | ✅ |
| **1A** | ユーザ文字定義 | ✅ KCGアクセス | ❌ | 🔶 ゲーム用 |
| **1B** | KCGアクセスモード | ✅ `CRT_STS_FLAG`+MODE1 | ❌ | 🔶 ゲーム用 |
| **30** | 31kHz設定 | ✅ (CRT31KHZ) | ⬜ 不要 | ✅ |
| **40** | グラフィック表示ON | ✅ GDC SLAVE START | ✅ `outp(0xA2,0x0D)` | ✅ |
| **41** | グラフィック表示OFF | ✅ GDC SLAVE STOP | ✅ `outp(0xA2,0x0C)` | ✅ |
| **42** | 表示領域設定 | ✅ `bios0x18_42(CH)` | ❌ ack | 🔶 ゲーム用 |
| **43** | パレット設定 | ✅ デジタルパレットI/O | ❌ ack | 🔶 ゲーム用 |
| **44** | ボーダカラー | ✅ NOP | ⬜ | ✅ |
| **47-48** | 描画(直線/円) | ✅ LIOライブラリ | ❌ | 🔶 ゲーム用 |
| **49** | グラフィック文字 | ✅ | ❌ | 🔶 |
| **4A** | 描画モード設定 | ✅ GDC SYNC | ❌ | 🔶 |

---

## INT 1Ch — カレンダBIOS

**NP21/W**: `bios1c.c` (~50行) / **OS32**: `v86_bios.c` (~50行)

| AH | 機能 | NP21/W | OS32 | ギャップ |
|----|------|--------|------|---------|
| **00** | 日時読出 | ✅ `calendar_get()` → ES:BX | ✅ `rtc_read()` → ES:BX | ✅ |
| **01** | 日時設定 | ✅ `calendar_set()` | ✅ ack | ✅ |
| **02** | インターバルタイマ設定 | ✅ IVT 1Ch書替 + PIT設定 | ❌ | 🔶 ゲーム用 |
| **03** | インターバルタイマ継続 | ✅ PIT設定 | ❌ | 🔶 |

---

## INT 09h — キーボードIRQ

**NP21/W**: `bios09.c` (~80行) — スキャンコード→BDAキーバッファ変換
**OS32**: IRQ注入方式 (v86.c `v86_kbd_enqueue()`)

| 処理 | NP21/W | OS32 | ギャップ |
|------|--------|------|---------|
| スキャンコード→キーコード変換 | ✅ シフトテーブル(0x0E00)参照 | 🔶 IRQ注入でROMに委譲 | ✅ 方式が異なるが動作 |
| BDA KB_COUNT更新 | ✅ `mem[MEMB_KB_COUNT]` | 🔶 ROM側で処理 | ✅ |
| シフトキー状態更新 | ✅ `KB_KY_STS` + LED | 🔶 `kbd_key_pressed[]` | ✅ |

---

## INT 11h/12h — 機器構成/メモリサイズ

| INT | NP21/W | OS32 | ギャップ |
|-----|--------|------|---------|
| 11h | BDA参照 | ✅ 0x0000固定 | ✅ |
| 12h | BDA 0x0413参照 | ✅ BDA 0x0413参照 | ✅ |

---

## INT 29h — DOS高速1文字出力

| 処理 | NP21/W | OS32 | ギャップ |
|------|--------|------|---------|
| CR/LF/BS/通常文字 | (DOS内部) | ✅ TVRAM直書 | ✅ |

---

## BDA (BIOS Data Area) 重要アドレス

NP21/W `biosmem.h` で定義。OS32 `v86_mem.c` での初期化状態:

| アドレス | 名称 | NP21/W初期値 | OS32設定 | ギャップ |
|---------|------|-------------|---------|---------|
| 0x0401 | EXPMMSZ | extmem<<3 | ❌ 未設定 | 🔶 |
| 0x0480 | SYS_TYPE | 0x03 (386+) | ❌ 未設定 | ⚠️ **IO.SYS参照** |
| 0x0482 | DISK_EQUIPS | equip bits | ✅ v86_mem.c | ✅ |
| 0x0493 | F2HD_MODE | 0xFF | ❌ 未設定 | ⚠️ **密度判定に使用** |
| 0x0500 | BIOS_FLAG0 | 0x01 | ❌ 未設定 | 🔶 |
| 0x0528 | KB_COUNT | 0 | ✅ (ROM経由) | ✅ |
| 0x053A | SHIFT_STS | 0 | ✅ (ROM経由) | ✅ |
| 0x053B | CRT_RASTER | 0x0F | ❌ 未設定 | 🔶 INT18h 0Ah使用 |
| 0x053C | CRT_STS_FLAG | mode dependent | ❌ 未設定 | 🔶 |
| 0x055C | DISK_EQUIP | FDD装備ビット | ✅ v86_mem.c | ✅ |
| 0x055E | DISK_INTL | IRQ完了フラグ | ✅ HLE更新 | ✅ |
| 0x0564 | DISK_RESULT | FDC結果(8B×4) | ✅ HLE更新 | ✅ |
| 0x0584 | DISK_BOOT | ブートデバイス | ✅ v86_mem.c | ✅ |
| 0x05AE | F144_SUP | 1.44MB対応 | ❌ 未設定 | 🔶 |
| 0x05CA | F2DD_MODE | 0xFF | ❌ 未設定 | ⚠️ **密度判定に使用** |
| 0x05CC | F2DD_P_OFF/SEG | パラメータポインタ | ✅ NP21/W値設定 | ✅ |
| 0x05F8 | F2HD_P_OFF/SEG | パラメータポインタ | ✅ NP21/W値設定 | ✅ |

---

## 優先修正リスト

### P1 (FreeDOSブートに必須)

1. **BDA 0x0480 SYS_TYPE = 0x03** — IO.SYSがCPU種別を参照
2. **BDA 0x0493 F2HD_MODE = 0xFF** — FDD密度自動判定に使用
3. **BDA 0x05CA F2DD_MODE = 0xFF** — 同上
4. **INT 1Bh 03h: `fddbios_equip()`** — BDA 0x055C の装備ビット正しく更新
5. **INT 1Bh 01h VERIFY** — READと同一ロジック(データ破棄)で追加

### P2 (安定性向上)

6. **INT 1Bh 02h 診断読み** — READにフォールスルー
7. **INT 1Bh 0Eh 密度設定** — BDA F2HD/F2DD_MODE更新
8. **INT 18h 16h** — DL(文字コード)/DH(アトリビュート)パラメータ対応
9. **INT 18h 17h/18h** — ブザーポート操作追加

### P3 (ゲーム互換性)

10. INT 18h 42h/43h パレット
11. INT 18h 1Ah ユーザ文字定義
12. INT 1Ch 02h/03h インターバルタイマ
