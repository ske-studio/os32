# VDM Phase 2 残り / Phase 3 — 技術調査レポート

実装計画書 v5 の未着手項目に必要なハードウェア仕様・BDA定義・I/O仕様をまとめる。

---

## 1. Phase 2 残りタスク一覧と必要情報

| # | タスク | 状態 | 主要な技術的課題 |
|---|--------|------|-----------------|
| 1 | FreeDOS(98) KERNEL.SYS ローダー | 未着手 | VFS→V86メモリへのロード方法 |
| 2 | INT 1Bh ディスクBIOS → VFS ブリッジ | 未着手 | AH別ファンクション実装 |
| 3 | INT 08h タイマ割り込みリフレクト | 未着手 | V86仮想割り込みインジェクション |
| 4 | INT 09h キーボード割り込みリフレクト | 未着手 | 同上 + BDAキーバッファ |
| 5 | PIT仮想化 (Counter#0) | 未着手 | モード設定・カウンタ読み出し |
| 6 | INT 11h / INT 12h | 未着手 | BDA応答のみ (軽量) |
| 7 | GRCG OFF + パレットリセット + 画面リストア | 未着手 | DOS→OS32復帰時の画面回復 |

> ✅ 完了済み: PIC仮想化, INT 1Ch カレンダBIOS, OUT F0h リブート検知

---

## 2. INT 1Bh ディスクBIOS — VFSブリッジ仕様

出典: PC9800Bible §2-9, UNDOCUMENTED memsys.md (DISK_EQUIP)

### 2.1 FreeDOS(98) ブートに最低限必要なファンクション

| AH | 機能 | 入力 | 出力 | 実装方針 |
|----|------|------|------|----------|
| `03h` | Initialize | AL=DA/UA | AH=0(成功) | ack のみ |
| `04h` | Sense (メディア検知) | AL=DA/UA | AH=0, BX=メディアバイト | 固定値応答 |
| `05h` | Read Sector | AL=DA/UA, BX=転送バイト数, CX=セクタ長(コード), DH=シリンダ, DL=セクタ, ES:BP=バッファ | AH=0(成功) | **VFS経由読み込み** |
| `06h` | Write Sector | 同上 | AH=0(成功) | VFS経由書き込み (Phase 3) |
| `84h` | Sense (拡張) | AL=DA/UA | AH=0 | 固定値応答 |

### 2.2 DA/UA フォーマット

```
AL レジスタ:
  bit 7-4: DA (Device Address) — デバイス種別
    9xh = 1MB FDD
    8xh = SASI/IDE HDD
    Axh = SCSI HDD
  bit 3-0: UA (Unit Address) — ユニット番号 (0-3)
```

### 2.3 VFSブリッジの設計

FreeDOS(98)のKERNEL.SYSとCOMMAND.COMをOS32のVFS上のファイルとして管理し、
INT 1Bh AH=05h (Read) をトラップして VFS → V86メモリにデータを転送する。

**方式A: FATイメージファイル方式** (推奨)
- HDDパーティションイメージ (FAT16) をファイルとして用意
- CHS→LBA変換してイメージ内のセクタを読み出し
- FreeDOS(98)のファイルシステムドライバがそのまま動作

**方式B: ファイル単位ブリッジ方式**
- INT 21h のファイルI/OをOS32 VFSに直接マッピング
- KERNEL.SYS側の大幅な改修が必要 → 非推奨

### 2.4 BDA DISK_EQUIP (0000:055Ch) 設定

```
0000:055Ch = 0x01  (1MB FDD UNIT#0 接続)
0000:055Dh = 0x00  (HDD なし)
```

FreeDOS(98)をFDDイメージからブートする場合の設定。
HDDイメージの場合は `055Dh bit 0 = 1` (SASI HDD UNIT#0)。

---

## 3. INT 08h タイマ割り込みリフレクト

出典: UNDOCUMENTED io_tcu.md, io_pic.md

### 3.1 仕組み

PC-98のタイマ割り込み (INT 08h) は PIT Counter#0 → マスタPIC IR0 で 100Hz で発生。

V86モード中、OS32の `timer_handler` (IRQ0) が呼ばれた際に、
V86タスクの仮想IFが有効であれば、V86のIVT経由で INT 08h をリフレクトする。

### 3.2 仮想割り込みインジェクション手順

```
1. timer_handler 内で v86_active をチェック
2. v86_active && v86_virtual_if の場合:
   a. V86スタックに FLAGS/CS/IP を push (リアルモードINTと同等)
   b. V86の CS:IP を IVT[08h] のハンドラアドレスに変更
   c. 仮想IF をクリア
   d. IRET で V86 に戻る (新しい CS:IP = INT 08h ハンドラ)
3. V86内のINT 08hハンドラが処理 → IRET → 元のコードに復帰
```

### 3.3 実装上の注意

- IRQスタブからの割り込みリフレクトは、`v86_gp_handler` とは別パスで処理する必要がある
- `isr_stub.asm` の `irq_stub_0` にV86リフレクト判定を追加
- V86スタックフレームの操作は `regs[]` 配列経由で行う
- **タイマ割り込みのネストに注意** (Phase 1-2レポートの教訓)

---

## 4. INT 09h キーボード割り込みリフレクト

出典: UNDOCUMENTED io_kb.md, memsys.md (KB_BUF)

### 4.1 PC-98 キーボードI/F

- チップ: 8251A相当 (19200bps固定)
- I/Oポート:
  - `0x41`: データ読み出し/コマンド書き込み (I/Oスルー許可済み)
  - `0x43`: ステータス/コマンドワード (I/Oスルー許可済み)
- 割り込み: マスタPIC IR1 → INT 09h

### 4.2 リフレクト方式

INT 08h と同様、IRQ1発生時に V86 の IVT[09h] へリフレクト。
キーボードI/Oポート (0x41, 0x43) は既にI/Oビットマップで許可済みなので、
FreeDOS(98)のキーボードハンドラがそのまま 8251A を直接操作できる。

### 4.3 BDA キーバッファ (0000:0502h-0521h)

FreeDOS(98)のキーボードBIOSが使用するFIFOバッファ (16組×2バイト)。
V86メモリ空間のバッキングRAM上に配置される (物理 0x300502-0x300521)。

初期化は FreeDOS(98) KERNEL.SYS が行うため、OS32側では特に設定不要。

---

## 5. PIT仮想化 (Counter#0)

出典: UNDOCUMENTED io_tcu.md

### 5.1 トラップ対象ポート

| ポート | 機能 | R/W |
|--------|------|-----|
| `0x71` (+ `0x3FD9`) | Counter#0 R/W | R/W |
| `0x77` (+ `0x3FDF`) | コマンドレジスタ | W |

### 5.2 仮想化内容

```c
/* 仮想PITステート */
struct v86_pit {
    u8  mode;           /* モード (通常 mode3 = 方形波) */
    u8  rw_mode;        /* 00=ラッチ, 01=LSB, 10=MSB, 11=LSB→MSB */
    u16 reload_value;   /* カウンタリロード値 */
    u8  latch_lo;       /* ラッチされたカウンタ下位 */
    u8  latch_hi;       /* ラッチされたカウンタ上位 */
    u8  read_phase;     /* 次のINで返すバイト (0=lo, 1=hi) */
    u8  write_phase;    /* 次のOUTで受けるバイト (0=lo, 1=hi) */
    u8  latched;        /* ラッチ済みフラグ */
};
```

### 5.3 コマンドレジスタ (0x77) デコード

```
bit 7,6: SC1,SC0 — カウンタ選択 (00=Counter#0, 01=Counter#1, 10=Counter#2)
bit 5,4: RL1,RL0 — R/Wモード (00=ラッチ, 01=LSB, 10=MSB, 11=LSB→MSB)
bit 3-1: M2-M0 — モード (011=mode3 方形波)
bit 0:   BCD (0=バイナリ)
```

FreeDOS(98)は通常 Counter#0 を mode3, バイナリ, LSB→MSB で初期化する。
カウンタ値は 100Hz (= 1.9968MHz / 19968 = 0x4E00) がデフォルト。

### 5.4 IN 0x71 の応答

ラッチコマンド未発行時: 現在のカウンタ値を推定して返す (tick_count ベース)。
ラッチ済み: ラッチ値を返し、ラッチフラグをクリア。

---

## 6. INT 11h / INT 12h

### 6.1 INT 11h — 機器構成取得

```
出力: AX = 機器構成フラグ
  bit 15-14: RS-232Cの数
  bit 11:    プリンタ接続あり
  bit 5-4:   FDD台数 (0=1台, 1=2台...)
  bit 2:     コプロセッサあり
```

V86実装: `AX = 0x0000` (最低限の構成) を返すか、BDA 0x0400-0x0401 を参照。

### 6.2 INT 12h — メモリサイズ取得

```
出力: AX = コンベンショナルメモリサイズ (KB単位)
```

V86実装: BDA 0x0413 (WORD) の値を返す。v86_mem.c で 640 に設定済み。

---

## 7. Phase 3 タスク — VZ Editor 対応

### 7.1 INT 18h テキストBIOS拡充

出典: PC9800Bible §2-6 テキストBIOS一覧

| AH | 機能 | 現状 | VZ Editor要否 |
|----|------|------|--------------|
| `0Ah` | テキスト画面モード設定 | ✅ ack | ○ |
| `0Bh` | テキスト画面モード取得 | ✅ | ○ |
| `0Ch` | テキスト画面表示開始 | ✅ ack | ○ GDC STARTコマンド発行 |
| `0Dh` | テキスト画面表示停止 | ✅ ack | ○ GDC STOPコマンド発行 |
| `0Eh` | 表示開始アドレス設定 | ❌ 未実装 | △ スクロール時に必要 |
| `0Fh` | カーソル位置取得 | ✅ | ○ |
| `10h` | カーソルブリンク設定 | ✅ ack | ○ |
| `11h` | カーソル表示開始 | ✅ ack | ○ GDC CSRWコマンド |
| `12h` | カーソル表示停止 | ✅ ack | ○ |
| `13h` | カーソル位置設定 | ✅ | ○ GDC CSRWコマンド |
| `14h` | フォントパターン読み出し | ❌ 未実装 | △ CGウィンドウ経由 |
| `16h` | テキストVRAMクリア | ✅ | ○ |

### 7.2 INT 18h 実装詳細メモ

**AH=0Ch (テキスト画面表示開始)**:
```c
/* GDC STARTコマンド (I/Oポート 0x62 に 0x0D を出力) */
outp(0x62, 0x0D);
```

**AH=0Dh (テキスト画面表示停止)**:
```c
/* GDC STOPコマンド (I/Oポート 0x62 に 0x0C を出力) */
outp(0x62, 0x0C);
```

**AH=11h (カーソル表示開始) — GDC CSRWによるカーソル位置反映**:
```c
/* GDC CSRWコマンド (0x49) でカーソルアドレス設定 */
u32 ead = v86_cursor_y * 80 + v86_cursor_x;
while (!(inp(0x60) & 0x04)); /* FIFO EMPTY待ち */
outp(0x62, 0x49);             /* CSRWコマンド */
outp(0x60, ead & 0xFF);       /* EAD下位 */
outp(0x60, (ead >> 8) & 0xFF); /* EAD上位 */
outp(0x60, 0x00);              /* dAD=0 */
```

**AH=14h (フォントパターン読み出し)**:
- CGウィンドウ (0xA4000-) にマッピング済み (R/O) のため、V86プログラムが直接読める
- BIOS経由の場合は DX=文字コード, BX:CX=バッファアドレスを使って転送

### 7.3 VZ Editor のキーボード操作

VZ Editorは **キーボード8251直接ポーリング** (I/O 0x41/0x43) を使用する。
これらのポートは既にI/Oビットマップで許可済み (v86_mem.c) なので、
追加のエミュレーション不要。

ただし、**INT 09h (キーボード割り込み)** のリフレクトは必須。
VZはBIOSキーバッファ (BDA 0x0502-0x0521) も併用するため。

### 7.4 ビープ音エミュレーション

出典: UNDOCUMENTED io_tcu.md (Counter#1), io_syste.md (0x35)

```
PIT Counter#1 (0x73/0x3FDB): ビープ音周波数設定 (mode3, デフォルト2kHz)
System Port C (0x35) bit 3: BUZ — 0=鳴動, 1=停止
System Port CMD (0x37): 06h=鳴動, 07h=停止
```

V86実装:
- Counter#1 への書き込み: 仮想レジスタに保存 (実音は出さない or OS32ビーザー経由)
- 0x35/0x37: BUZビットをトラップして仮想ステートに記録

### 7.5 VSYNC割り込み (INT 0Ah) リフレクト

- マスタPIC IR2 → INT 0Ah
- CRT割り込みリセット (I/O 0x64 への書き込み) で次のVSYNCでIRQ発生
- I/O 0x64 は既にスルー許可済み
- VZ Editorがカーソル点滅等でVSYNCを使用する可能性あり

---

## 8. DOS→OS32 復帰時の画面リストア手順

出典: PC9800Bible §2-6 モードFF, §2-7 グラフィック

```c
void v86_restore_screen(void)
{
    /* 1. GRCG OFF */
    outp(0x7C, 0x00);

    /* 2. EGC OFF → GRCG互換モードに戻す */
    outp(0x6A, 0x07);  /* 拡張モード変更可 */
    outp(0x6A, 0x04);  /* GRCG互換モード */
    outp(0x6A, 0x06);  /* 拡張モード変更不可 */

    /* 3. 16色モード復帰 */
    outp(0x6A, 0x01);  /* 16色モード */

    /* 4. パレットリセット (デフォルト16色) */
    /* パレット0=黒(0,0,0), パレット7=白(7,7,7)... */
    /* OS32のデフォルトパレットテーブルを設定 */

    /* 5. テキストGDC: 80桁×25行モード */
    outp(0x68, 0x04);  /* 80桁モード */
    outp(0x68, 0x07);  /* ANK 7×13ドット */
    outp(0x68, 0x0F);  /* 画面表示可 */

    /* 6. テキストVRAM / コンソール再初期化 */
    /* console_init() 等を呼び出し */
}
```

---

## 9. 実装優先度と依存関係

```mermaid
graph TD
    A[INT 11h/12h<br/>軽量] --> B
    B[INT 1Bh ディスクBIOS<br/>VFSブリッジ] --> C
    C[KERNEL.SYS ローダー] --> D
    E[PIT仮想化] --> D
    F[INT 08h タイマリフレクト] --> D
    G[INT 09h KBリフレクト] --> D
    D[FreeDOS(98) ブート]
    D --> H[画面リストア]
    D --> I[Phase 3: VZ Editor]
```

推奨実装順:
1. INT 11h / INT 12h (最小工数)
2. PIT仮想化 (Counter#0 モード設定+読み出し)
3. INT 08h タイマ割り込みリフレクト (仮想割り込みインジェクション基盤)
4. INT 09h キーボード割り込みリフレクト
5. INT 1Bh ディスクBIOS → VFS ブリッジ
6. KERNEL.SYS ローダー
7. 画面リストア
8. Phase 3 (テキストBIOS拡充 → VZ Editor)

---

## 10. 出典一覧

| 出典 | 参照項目 |
|------|---------|
| PC9800Bible §2-6 | テキストVRAM, GDC, CRTC, テキストBIOS (INT 18h) |
| PC9800Bible §2-7 | グラフィックVRAM, GRCG, パレット, モードFF |
| PC9800Bible §4-3 | I/Oポートマップ全般 |
| PC9800Bible §4-5 | 割り込みベクタ一覧 |
| PC9800Bible §4-7 | MS-DOS ファンクションコール一覧 |
| PC9800Bible §2-9-1 | IDE PIO, INT 1Bh BIOSセクタ番号仕様 |
| UNDOC io_pic.md | PIC初期化 (ICW1-4), EOI手順, マスタ/スレーブ接続図 |
| UNDOC io_tcu.md | PIT Counter#0-2, クロック周波数, モード設定 |
| UNDOC io_kb.md | キーボード8251A, メイク/ブレイクコード |
| UNDOC io_syste.md | システムポート 8255A, ブザー制御 (0x35/0x37) |
| UNDOC memsys.md | BDA全定義, DISK_EQUIP, KB_BUF, BIOS_FLAG |
