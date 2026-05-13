# 6. BIOSエミュレーション

ソース: [`v86_bios.c`](../../../kernel/v86_bios.c), [`v86_disk.c`](../../../kernel/v86_disk.c)

## 6.1 エミュレーション方式

V86ゲストが `INT n` を実行すると #GP が発生し、GPハンドラ内で
割り込み番号ごとにBIOSエミュレータを呼び出す。

```c
case 0xCD: /* INT n */
    intno = code[1];
    switch (intno) {
    case 0x18: return v86_bios_int18(regs);
    case 0x1B: return v86_bios_int1b(regs);
    case 0x1C: return v86_bios_int1c(regs);
    case 0x11: ... case 0x12: ...
    case 0x29: return v86_bios_int29(regs);
    default:   /* IVT経由でゲストハンドラに転送 */
    }
```

エミュレートしない割り込みは IVT (ゲストが設定したベクタ) に転送される。
DOSカーネルが INT 21h 等を自前でフックしているため、そのまま機能する。

## 6.2 INT 18h — テキスト/キーボード/グラフィックBIOS

### AH=00h: キーボード入力 (ブロッキング)

OS32の `kbd_trygetkey()` でスキャンコード+ASCII を取得。
キーが押されるまで HLT ループで待機。

```
戻り値: AH = スキャンコード, AL = ASCIIコード
```

### AH=01h: キーバッファ状態確認 (peek)

BDAのキーボードバッファ (0x502-0x522) のHEAD ≠ TAIL で判定。
BH=スキャンコード, BL=ASCIIコードを返す (バッファから取り出さない)。

### AH=02h: シフトキー状態

```
戻り値: AL = kbd_shift_state
  bit0: CTRL     bit1: SHIFT
  bit2: カナ     bit3: GRPH
  bit4: CAPS     bit5: ナムロック
```

### AH=0Ah: テキスト画面モード設定

ack応答 (80×25固定)。カラー/モノクロの切り替えは未実装。

### AH=0Fh: カーソル位置取得

GDCのCSRWレジスタからカーソルアドレスを取得し、X/Y座標に変換。

```
戻り値: DH = X (列, 0-79), DL = Y (行, 0-24)
```

### AH=13h: カーソル位置設定

X/Y → GDCカーソルアドレス (Y*80+X) に変換し、GDC CSRWコマンド発行。

### AH=14h: TVRAM 1文字書き込み

指定位置に文字コード(2バイト) + アトリビュート(2バイト) を書き込み。

```
入力: DH=X, DL=Y, BL:BH=文字コード, AL=アトリビュート
TVRAM[Y*80+X] = BL | (BH<<8)
ATTR[Y*80+X] = AL | (attr_high<<8)
```

### AH=16h: TVRAMクリア (画面クリア)

指定範囲のTVRAMを空白 (0x0020) + デフォルトアトリビュート (0xE1) で埋める。

### AH=1Ah: テキスト行スクロールアップ

TVRAM上でメモリコピーによるスクロール。最終行をクリアで埋める。

### AH=40h/41h: グラフィック画面表示開始/停止

I/O 0xA4-0xA6 経由でGDC SYNC制御。グラフィック表示レイヤのON/OFF。

## 6.3 INT 1Bh — ディスクBIOS

### レジスタ規約

```
[入力]
AH = ファンクション (上位ニブル=フラグ, 下位ニブル=機能)
AL = DA/UA (0x90=1MB FDD UNIT#0, 0x30=2DD FDD UNIT#0)
BX = 転送バイト数
CH = セクタ長コード (0=128, 1=256, 2=512, 3=1024)
CL = シリンダ番号 (0-76)
DH = ヘッド番号 (0-1)
DL = セクタ番号 (1ベース)
ES:BP = 転送バッファアドレス

[出力]
AH = ステータス (0=成功)
CF = 0:成功, 1:エラー
```

### AHフラグビット

```
bit7 (0x80): MFMフラグ (HD=MFM, DD=FM)
bit6 (0x40): Multi-track
bit5 (0x20): MFM/FM
bit4 (0x10): SEEK — SEEKフラグ付きREAD (fdc_treg更新)
```

### 機能00h: シーク/リセット

AH bit4が立っている場合、CL値で`fdc_treg`を更新。

### 機能01h/06h: READ

3つのバックエンドをサポート:

1. **RAW/FDI**: `loop_dev_read_chs()` でCHS→LBA変換+読み取り
2. **D88**: `loop_dev_seek_d88()` でトラックテーブル参照+セクタID照合
3. **実FDD**: `fdc_read_sector_geom()` で物理FDCアクセス

D88モードの特殊処理:
- トラック選択は `fdc_treg + fdc_hd` (SEEK物理位置)
- セクタID照合は `CL + DH + R` (コマンドパラメータ)
- Ys等のコピープロテクション: SEEK位置とコマンドCHが意図的に異なる
- `fdc_hd = (DH XOR (DA>>2)) & 1` (NP21/W bios1b.c準拠)

BDA FDC結果バッファ (0000:0564) を更新:
```
ST0, ST1, ST2, C, H, R, N, NCN
```

### 機能04h: センス

```
戻り値 AH:
  bit0: 2HDメディア (AL bit7=1の場合)
  bit3: 1MB/640KB互換ドライブ (AH=84hの場合)
  bit4: ライトプロテクト中
```

ライトプロテクト: vfs_fstatで書き込み権限を確認 (`OS_S_IWUSR`)。

### 機能05h: WRITE

READと対称な実装。D88モードではloop_dev_write_chs。

## 6.4 INT 1Ch — カレンダBIOS

AH=00h: RTC読み出し。`rtc_read()` で現在日時を取得し、BCD形式で返却。

```
戻り値: CX:DX = BCD日時 (年月日時分秒)
```

## 6.5 INT 11h — 機器構成

```
戻り値: AL = 設備フラグ
  bit0-3: FDD台数 (1)
  bit4:   CRT接続 (1)
  他は最小構成
```

## 6.6 INT 12h — メモリサイズ

BDA 0x0413 を参照して640KBを返却。

## 6.7 INT 29h — DOS高速1文字出力

TVRAM直接書き込みによる1文字出力。INT 18h AH=14hより高速。

対応制御文字:
- 0x0D (CR): カーソルX=0
- 0x0A (LF): カーソルY++、最終行でスクロール
- 0x08 (BS): カーソルX-- (先頭でラップなし)

## 6.8 未実装BIOS割り込み

| INT | 機能 | PC/AT 用途 | PC-98 用途 | DOS5 NEC版での呼出可能性 | 対策 |
|-----|------|-----------|-----------|----------------------|------|
| 1Ah | タイマBIOS | システムタイマカウント取得 | **未使用** (PC-98 は INT 08h でカウント、INT 1Ch でカレンダ) | 🟢 低 — NEC版 DOS5 は呼ばない設計。PC/AT 互換のために IO.SYS が念のため呼ぶ可能性は要確認 | 当面不要。v86_trace で呼出を観測した時点で ack 応答実装 |
| 1Eh | プリンタBIOS | PRN 出力 / 状態取得 | 同上 | 🟡 中 — CONFIG.SYS の DEVICE=PRN 等で呼ばれる可能性 | ack 応答実装 (戻り値 AH=0) で十分な見込み |
| 10h | CRTモードBIOS | テキスト/グラフィックモード設定 (VGA/EGA) | **PC-98 では原則不使用** (INT 18h でモード設定) | 🟢 低 — DOS本体では使わない。ただし古いPC/AT互換志向のソフトが念のため呼ぶ可能性 | 当面不要。v86_trace 監視 |
| 20h | DOS Terminate (PSP終了) | ✅ V86 終了 (非ネイティブ) | ✅ 同上 | ✅ 実装済み ([§2.3](02_cpu_emulation.md)) | — |
| 21h AH=4Ch | Exit Process | ✅ V86 終了 (非ネイティブ) | ✅ 同上 | ✅ 実装済み | — |
| 33h | マウス (DOSマウスドライバ) | バスマウス / シリアル | バスマウス I/O 経由 | 🟢 低 (アプリケーション層) — [§9.3 Phase 3-4](09_msdos_roadmap.md) | 別フェーズで対応 |

### 6.8.1 検証アクション

未実装 INT が DOS5 ハングの原因か判別するため:

1. **v86_trace** 拡張で全INT呼出を時系列で記録 ([11.7.2](11_gaps_and_verification.md#1172-int-18h-トレース取得手順-仕様のみ))
2. DOS5 ブート→ハング再現後、Ctrl+GRPH+DEL で脱出 → ログ取得
3. ハング直前 ~100件のシーケンスから「未実装INTがダミーIVT (`0x003F:0x0000`) 経由
   で IRET された後にゲストが停止する」パターンを検出
4. 検出された INT 番号に対し:
   - **ack 応答で済む系** (INT 1Eh, INT 1Ah等) → `v86.c` のディスパッチに追加
   - **データ返却必要系** → `v86_bios.c` に専用ハンドラ追加

詳細は [11.2 A4](11_gaps_and_verification.md#a4-int-18h-呼び出しトレース取得) / [11.3 B3-B5](11_gaps_and_verification.md#113-b-中優先度--dosが起動した後で必要) 参照。
