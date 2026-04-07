# プロテクトモード IDE PIO 読み込み実証実験

> **実験日**: 2026-04-04
> **目的**: 32bitプロテクトモード (PM) 移行後に、IDE PIOポート直接操作でHDDからセクタを読み込めるかの技術実証

---

## 1. 背景

OS32のHDDブートシステムでは、IPL→ローダー→カーネルの3段階ロードを行う。
現行のIPLとローダーはリアルモードでBIOS INT 1Bhを使用してディスクアクセスを行っているが、
PM移行後はBIOSが使用できなくなるため、カーネルの追加セクタロードやファイルシステムアクセスが不可能になる。

**目標**: PM内でIDE I/Oポートを直接操作する32bit PIOリードが動作するかを検証し、
完全32bitブートアーキテクチャの技術的裏付けを取る。

### 技術的根拠

IDE I/OポートはI/O空間 (0x640-0x64E) に存在するため、
Ring 0 (CPL=0) のプロテクトモードでも `in`/`out` 命令で直接アクセス可能。
I/Oパーミッションビットマップ (IOPB) の設定なしでRing 0からは全I/Oポートにアクセスできる。

---

## 2. テスト構成

| 項目 | 内容 |
|------|------|
| IPL | `boot_hdd_bak_bios.asm` (INT 1Bh BIOS版) |
| ローダー | `test_pm_pio.asm` (新規作成) |
| テスト環境 | NP21/W x64 + test.nhd (C=75, H=8, S=17) |
| アセンブラ | NASM |
| テスト手法 | 複数のCHSアドレスで読み込み、NHD上の既知データと照合 |

### テストフロー

```
IPL (16bit, INT 1Bh)
  ↓ LBA 2-5 → 0000:8000 にローダーロード
  ↓ far jmp 0000:8000
ローダー (16bit → 32bit)
  ↓ A20有効化 → GDTロード → CR0.PE=1
  ↓ far jmp to pm_entry32
PM (32bit, Ring 0)
  ↓ IDE SRST (ソフトリセット)
  ↓ CHS指定でセクタ読み込み × 6パターン
  ↓ 先頭バイトをTVRAMにHEXダンプ
  ↓ HLT
```

---

## 3. 診断結果

### 3.1 CHS マッピング解析

NHD上の既知データ:
- Sector 0 (IPL): `EB 09 90 90 49 50 4C 31`
- Sector 1 (パーティション): `80 E2 ...`
- Sector 2 (ローダー自身): `EB 01 90 31 C0 8E D8 8E`
- Sector 6 (カーネル): `BF 20 D9 01 00 B9 EC E3`

**読み込み結果:**

| CHSセクタ番号 | 実際に読めたデータ先頭 | 対応するLBA | 判定 |
|---|---|---|---|
| 0 | `ERR:04` (ABRT) | — | ❌ 無効値 |
| 1 | `EB 09 90 90 49 50 4C 31` | LBA 0 | ✅ IPL |
| 2 | `80 E2 00 00 00 00 00 00` | LBA 1 | ✅ パーティションテーブル |
| 3 | `EB 01 90 31 C0 8E D8 8E` | LBA 2 | ✅ ローダー |
| 7 | `BF 20 D9 01 00 B9 EC E3` | LBA 6 | ✅ カーネル |
| 6 | `00 00 00 00 00 00 00 00` | LBA 5 | ✅ 未使用セクタ |

**結論: CHS sector N → LBA (N-1)**

NP21/WのIDEエミュレーションはATA標準に準拠し、CHSセクタ番号は**1ベース**。
セクタ番号0はATA仕様上無効であり、ABRTエラー (ステータスbit2) が返される。

---

## 4. 発見事項

### 4.1 LBAモードは NP21/W IDE で動作しない

Drive/Headレジスタ (0x64C) のbit6を1にしてLBAモード (0xE0) を指定しても、
正しいセクタが読めない。異なるLBA値を指定しても同一の不正データが返される。

```asm
;; ❌ 動作しない
mov     dx, 64Ch
mov     al, 0E0h          ;; LBA mode
out     dx, al

;; ✅ 正しく動作する
mov     dx, 64Ch
mov     al, 0A0h          ;; CHS mode
out     dx, al
```

**対策**: 常にCHSモード (0xA0) を使用し、LBA→CHS変換を行う。

### 4.2 PM移行後の IDE SRST が必須

BIOS INT 1BhがIDEコントローラの内部状態に影響を与えるため、
PM移行後に初めてPIOアクセスする前にIDEソフトリセット (SRST) が必要。

SRSTなしの場合、コントローラが前回のBIOSリード時の内部バッファを返し、
異なるCHSアドレスでも同一データが読まれる現象が発生した。

```asm
;; IDE SRST 手順
;; Device Control Register: 0x74C (PC-98)
mov     dx, 074Ch
mov     al, 06h            ;; SRST=1, nIEN=1
out     dx, al

;; リセットホールド待ち (数万ループ)
mov     ecx, 10000h
.wait:  dec ecx
        jnz .wait

mov     dx, 074Ch
mov     al, 02h            ;; SRST=0, nIEN=1
out     dx, al

;; BSYクリア待ち (最大数百万ループ)
mov     ecx, 200000h
.bsy:   mov dx, 64Eh
        in  al, dx
        test al, 80h
        jz  .done
        dec ecx
        jnz .bsy
.done:
```

### 4.3 LBA → CHS 変換

```asm
;; 入力: EAX = LBA
;; 使用定数: GEO_SPT = 17, GEO_HEADS = 8

xor     edx, edx
mov     ebx, GEO_SPT       ;; 17
div     ebx                 ;; EAX = LBA/SPT, EDX = LBA%SPT
inc     edx                 ;; sector = remainder + 1 (1-based)

push    edx                 ;; sector保存

xor     edx, edx
mov     ebx, GEO_HEADS      ;; 8
div     ebx                 ;; EAX = cylinder, EDX = head

;; IDEレジスタ出力:
;; 0x646 = sector (1-based)
;; 0x648 = cylinder low
;; 0x64A = cylinder high
;; 0x64C = (head & 0x0F) | 0xA0
```

---

## 5. デバッグ中に発見したバグ

### 5.1 test_p_boot.asm — PIO LBAレジスタ出力バグ

```asm
;; ❌ 修正前: AL = 1 (セクタカウント値) がそのままセクタ番号に出力される
mov     dx, 644h
mov     al, 1
out     dx, al          ;; Sector Count = 1
add     dx, 2           ;; 646h
mov     ebx, eax
out     dx, al          ;; ← AL=1 を出力してしまう！

;; ✅ 修正後
mov     ebx, eax        ;; LBAを先にEBXに退避
mov     dx, 644h
mov     al, 1
out     dx, al          ;; Sector Count = 1
mov     dx, 646h
mov     al, bl          ;; LBA[7:0]
out     dx, al
```

### 5.2 test_p_boot.asm — バッファアドレス未進行バグ

`pusha`/`popa` がDIを復元するため、複数セクタを連続ロードしても
全セクタが同じアドレスに上書きされ、最後のセクタのみが残る。

```asm
;; ✅ 修正: ESを0x20パラグラフ (512B) ずつ進める
.load_loop:
    call    read_sector_pio
    push    eax
    mov     ax, es
    add     ax, 20h
    mov     es, ax
    pop     eax
    ...
```

### 5.3 boot_hdd.asm — CHS PIO の EAX/AL 破壊バグ

CHS変換前に `mov al, 1` (セクタカウント) でEAXのALバイトが破壊され、
後続の `div ebx` が誤った値で計算される。

```asm
;; ❌ boot_hdd.asm 現行コード
mov     dx, 644h
mov     al, 1           ;; ← EAX の AL を破壊！
out     dx, al
xor     edx, edx
div     ebx             ;; EAX は不正な値
```

---

## 6. 完全32bitブートローダーへの設計指針

本実験により、以下のアーキテクチャが技術的に実現可能であることが確認された:

```
┌─────────────────────────────────────────────────────────────┐
│  IPL (512B, 16bit)                                         │
│  - BIOS INT 1Bh でローダー (LBA 2-5) を 0000:8000 にロード │
│  - far jmp 0000:8000                                       │
├─────────────────────────────────────────────────────────────┤
│  ローダー (2048B, 16bit → 32bit PM)                        │
│  - A20有効化、GDTロード、PM移行                              │
│  - IDE SRST                                                 │
│  - CHS PIOでカーネル全セクタロード (32bit)                    │
│  - jmp to kernel entry                                      │
├─────────────────────────────────────────────────────────────┤
│  カーネル (32bit PM)                                        │
│  - CHS PIOで追加セクタ/FSアクセス可能                        │
│  - BIOSに完全非依存                                         │
└─────────────────────────────────────────────────────────────┘
```

### 制約事項

1. **LBAモード不可** — 必ずCHS変換を行う
2. **ジオメトリ情報が必要** — IPLからローダーにH/SPTを渡すか、ローダーに定数埋め込み
3. **IDE SRSTが必要** — PM移行直後にリセットしてからPIOアクセス
4. **セクタ番号は1ベース** — `sector = (LBA % SPT) + 1`

---

## 7. 関連ファイル

| ファイル | 内容 |
|---------|------|
| `boot/test_pm_pio.asm` | テストコード本体 (CHS マッピング診断版) |
| `boot/boot_hdd_bak_bios.asm` | テスト用IPL (BIOS版) |
| `boot/test_p_boot.asm` | PIO版IPL (バグ修正済み、LBAモードのため未使用) |
| `tools/write_ipl.py` | NHDセクタ書き込みツール |
| `docs/HDD_BIOS_DEBUG.md` | INT 1Bh BIOSデバッグ記録 |
