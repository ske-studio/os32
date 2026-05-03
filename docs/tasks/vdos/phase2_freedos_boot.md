# Phase 2 進捗レポート — FreeDOS(98) V86 ブート

## 日付: 2026-05-02

---

## 1. 現在のステータス

**Phase 2: FreeDOS(98) ブート — IPL通過・カーネルロード完了・カーネル実行中にハング**

```
[完了] IPL (ブートセクタ) 実行
[完了] INT 1Bh ディスクBIOS でカーネルファイル読み出し
[完了] "FreeDOS(98) FAT Kernel" 表示
[問題] カーネル初期化中にハング (未特定の INT/IO 待ち)
[対策] fdkernel ソースビルド環境構築完了 → デバッグコード埋め込み段階へ
```

---

## 2. 実装済みコンポーネント

### 2.1 V86コア (Phase 0-1 完了)

| ファイル | 行数 | 内容 |
|----------|------|------|
| `v86.c` | 448 | #GPハンドラ (CLI/STI/HLT/INT/PUSHF/POPF/IRET/IN/OUT エミュレーション) |
| `v86.h` | 70 | V86コンテキスト定義 |
| `v86_entry.asm` | — | IRET による V86モード遷移 |
| `v86_test.c` | 326 | カーネル統合テスト (COM実行, FDDブート) |

### 2.2 V86 BIOS エミュレーション (Phase 1-2)

| ファイル | 行数 | 内容 |
|----------|------|------|
| `v86_bios.c` | 339 | INT 18h (CRT/KB), INT 29h (コンソール出力), INT 1Ch (カレンダ) |
| `v86_disk.c` | 264 | INT 1Bh ディスクBIOS (FDD 2HD 読み出し) |
| `v86_mem.c` | 410 | V86メモリ構築, BDA/IVT初期化, I/Oビットマップ設定 |
| `v86_pic.c` | 164 | PIC仮想化 (EOI/IMR/IRR) |
| `v86_pit.c` | 203 | PIT仮想化 (Counter#0/1/2 モード設定・読み出し) |

### 2.3 INT 1Bh ディスクBIOS 実装状況

| AH値 | 機能 | 状態 |
|------|------|------|
| `03h` | リセンス / リキャリブレート | ✅ 成功返却 |
| `04h` | センスデバイスステータス | ✅ ステータス返却 |
| `05h` | ドライブ初期化 | ✅ 成功返却 |
| `06h` | セクタ読み出し (識別モード) | ✅ VFS経由読み出し |
| `07h` | ベリファイ | ✅ 成功返却 |
| `46h/56h` | 2HD セクタ読み出し (256B/1024B) | ✅ CHS→LBA変換, 複数セクタ対応 |
| `84h` | センスドライブ種別 | ✅ 2HD (0x90) 返却 |

### 2.4 BDA (BIOS Data Area) 設定状況

| アドレス | 設定値 | 用途 |
|---------|--------|------|
| `0x400` | `0x23` | BIOS_FLAG2 (機種フラグ) |
| `0x458` | `0x38` | PRT_STS (タイマ) |
| `0x480` | `0x08` | CPU_FLAG |
| `0x484` | `0x03` | CPU_TYPE (i386) |
| `0x501` | `0x00` | BIOS_FLAG5 |
| `0x55C` | `0x01` | DISK_EQUIP (FDD#0 接続) |
| `0x564` | FDDパラメータ | DISK_RESULT |
| `0x584` | `0x90` | DA/UA (1MB FDD Unit#0) |
| `0x5AE` | `0xA0` | 640KB メモリ |

### 2.5 I/Oビットマップ (パススルー設定)

以下のポートは V86 から直接アクセス可能 (GP トラップなし):

- GDC テキスト: `60h-6Ah` (偶数)
- CRTC + GRCG: `70h-7Eh` (偶数)
- GDC グラフィック + パレット: `A0h-AEh` (偶数)
- EGC: `04A0h-04AEh` (偶数)
- FM音源: `0188h-018Eh` (偶数)
- キーボード 8251: `41h, 43h`

---

## 3. FreeDOS(98) ブートシーケンスの到達状況

```
IPL (fd98_2hd.img 先頭1024バイト)
  │
  ├── [✅] JMP 0x59 (BPBスキップ)
  ├── [✅] INT 1Bh AH=06h (FATテーブル読み出し)
  ├── [✅] INT 1Bh AH=56h (kernel.sys セクタ読み出し, 複数回)
  ├── [✅] "FreeDOS(98) FAT Kernel" 文字列表示 (INT 18h)
  ├── [✅] kernel.sys をメモリにロード
  ├── [✅] FAR JMP でカーネルエントリポイントへ
  │
  ▼
FreeDOS(98) カーネル (kernel.sys)
  │
  ├── [✅] カーネルヘッダ処理
  ├── [??] init_kernel() — 初期化開始
  ├── [??] init_oem() — OEM固有初期化
  ├── [??] ★ この付近でハング ★
  │        (特定のINT呼び出し or I/Oポート待ちでブロック)
  │
  ▼
  未到達: CONFIG.SYS処理, COMMAND.COM起動
```

---

## 4. ハング原因の仮説

カーネル実行中にGPトラップ制限 (1,000,000回) に達せずにハングする。
考えられる原因:

### 仮説 A: 未実装のINTでBIOS ROMの無限ループに陥る

IPL内で `INT 18h AH=00h` (キー入力待ち) が無限ループに入る問題を
既に修正済み（キー入力をトリガーしてV86終了）。
カーネル内でも同様の未実装BIOSファンクションが呼ばれている可能性。

### 仮説 B: I/Oポート直接アクセスで応答待ち

パススルー設定したI/Oポート (GDC等) に対して、
実機ハードウェアの応答を待つビジーループに入っている可能性。

### 仮説 C: タイマ割り込み (INT 08h) 未実装

FreeDOS カーネルの初期化ルーチンがタイマ割り込みによるタイムアウトを
期待しているが、V86環境ではまだタイマリフレクトが未実装。

---

## 5. デバッグ戦略

V86環境の外側からの監視（GPハンドラログ）には限界がある。
**FreeDOS(98)カーネル自体をソースからビルドし、内部にデバッグログを埋め込む**
アプローチに移行する。

### 5.1 ビルド環境 — 構築完了 ✅

```
lpproj/fdkernel (nec98test ブランチ)
  ├── コンパイラ: OpenWatcom V2 wcc (16bit)
  ├── リンカ: OpenWatcom V2 wlink
  ├── アセンブラ: NASM
  └── ホストツール: Linux GCC (exeflat)
```

ビルド手順の詳細: [fdkernel_build.md](fdkernel_build.md) を参照。

### 5.2 次のステップ

1. **デバッグコード挿入**: `main.c` / `initoem.c` にシリアルポート出力を追加
2. **カスタム kernel.sys 作成**: `-DDEBUG` 付きでビルド
3. **FDDイメージへの埋め込み**: 既存の fd98_2hd.img 内の kernel.sys を置換
4. **V86で実行**: ハング箇所を特定
5. **V86ハンドラ拡充**: 不足しているINT/IOを実装

---

## 6. 関連ファイル

### ソースコード

| ファイル | 内容 |
|----------|------|
| [v86.c](file:///mnt/c/WATCOM/src/os32/kernel/v86.c) | #GPハンドラ本体 |
| [v86_bios.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_bios.c) | BIOS INT エミュレーション |
| [v86_disk.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_disk.c) | INT 1Bh ディスクBIOS |
| [v86_mem.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_mem.c) | V86メモリ/BDA/IVT/IOビットマップ |
| [v86_pic.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_pic.c) | PIC仮想化 |
| [v86_pit.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_pit.c) | PIT仮想化 |
| [v86_test.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_test.c) | V86統合テスト |

### ビルド環境

| ファイル | 内容 |
|----------|------|
| [build98.sh](file:///mnt/c/WATCOM/src/os32/tools/fdkernel/nec98/build98.sh) | ハイブリッドビルドスクリプト |
| [fdkernel_build.md](fdkernel_build.md) | ビルド手順書 |

### FDDイメージ

| ファイル | 内容 |
|----------|------|
| `tools/freedos98/fd98_2hd.img` | FreeDOS(98) 2HD FDDイメージ (1.2MB) |
| `tools/fdkernel/nec98/bin/kernel.sys` | カスタムビルド版カーネル (90KB, DEBUG) |
