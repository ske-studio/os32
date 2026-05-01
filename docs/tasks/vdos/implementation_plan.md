# VDM (Virtual DOS Machine) — 実装計画書 v4

OS32シェルから `dos` コマンドで FreeDOS(98) を V86モードで起動し、
DOSコマンドプロンプトを提供する。`exit` でOS32シェルに復帰する。
テスト目標: **VZ Editor の起動**。

---

## 1. アーキテクチャ概要

### 動作モデル

```
OS32シェル (PM Ring0, Level 0)
    │
    │  "dos" コマンド実行
    ▼
FreeDOS(98) V86モード (仮想リアルモード)
  ├── KERNEL.SYS (INT 21h等を自前処理)
  ├── COMMAND.COM (freecom_dbcs2)
  └── DOSアプリケーション (VZ Editor等)
    │
    │  "exit" で復帰
    ▼
OS32シェルに戻る (longjmp)
```

### メモリレイアウト

```
物理メモリ:
  0x100000-0x1FFFFF : OS32カーネル帯域 (1MB)
  0x200000-0x2FFFFF : SQLite帯域 (1MB)
  0x300000-0x3FFFFF : シェル常駐 / DOSバッキングRAM (1MB, 排他使用)
  0x400000〜        : 未使用 (将来拡張用)

V86タスクの仮想アドレス空間 (1MB):
  0x00000-0x003FF : IVT (割り込みベクタテーブル)
  0x00400-0x004FF : BIOSデータエリア (仮想構築)
  0x00500-0x9FFFF : FreeDOSカーネル + ユーザ領域 (~636KB)
  0xA0000-0xA3FFF : テキストVRAM (実機に直接マッピング)
  0xA8000-0xBFFFF : グラフィックVRAM (実機に直接マッピング)
  0xE0000-0xE7FFF : グラフィックVRAM プレーン3 (実機に直接マッピング)
  0xF0000-0xFFFFF : BIOS ROM (実機ROMをR/Oマッピング)

ページング: 仮想 0x00000-0x9FFFF → 物理 0x300000-0x39FFFF
            仮想 0xA0000-0xFFFFF → 物理 0xA0000-0xFFFFF (アイデンティティ)
```

### VRAM方針

DOSモード中はVRAM + GRCG を**完全にDOSに明け渡す**。

- VRAM領域は実機ハードウェアに直接マッピング (バックバッファ不要)
- GRCG/EGC の I/Oポートもスルー (I/Oビットマップで許可)
- DOS→OS32復帰時にGRCGを確実にOFF + 画面状態をリストア

### INTトラップの2層構造

```
DOSアプリ → INT 21h → #GP → OS32: IVT参照 → FreeDOS(98)ハンドラに転送
                                              (V86内部で完結)
                                                ↓ ディスクI/O
                                              INT 1Bh → #GP → OS32: VFS経由処理
                                                ↓ 画面出力
                                              INT 18h → #GP → OS32: TVRAM操作
```

---

## 2. 言語分担

### 原則: ロジック層はRust、ハードウェア接触層はC/NASM

```
┌─────────────────────────────────────────────────────┐
│  NASM (isr_stub.asm, v86_entry.asm)                  │
│  - V86モード遷移 (IRET)                              │
│  - #GP スタブ (V86スタックフレーム処理)                │
│  - V86復帰 (IRETD)                                   │
├─────────────────────────────────────────────────────┤
│  C (gdt.c, tss.c, isr_handlers.c)                    │
│  - TSS構造体初期化、GDT拡張                          │
│  - #GPハンドラ: V86判定 → Rust呼び出し                │
├─────────────────────────────────────────────────────┤
│  Rust (os32_v86 crate)          ← ロジックの大部分   │
│  - V86命令デコーダ (match式)                         │
│  - INT番号ディスパッチ                               │
│  - BIOS INTエミュレーション                          │
│  - I/Oポートエミュレーション (PIC/PIT)                │
│  - DOSローダー (KERNEL.SYSロード・配置)               │
│  - V86コンテキスト管理                               │
└─────────────────────────────────────────────────────┘
```

### os32_v86 クレート構成

```
src/os32/kernel/v86/          (Rust crate: os32_v86)
  Cargo.toml
  .cargo/config.toml          (i686-os32-none ターゲット)
  src/
    lib.rs                    FFIエントリポイント (#[no_std])
    context.rs                V86Context 構造体
    decoder.rs                命令デコーダ (INT/CLI/STI/IN/OUT等)
    dispatch.rs               INT番号による振り分け
    bios/
      mod.rs                  BIOS INTエミュレーション統合
      disk.rs                 INT 1Bh (ディスクBIOS → VFS)
      video.rs                INT 18h (ビデオBIOS → TVRAM)
      keyboard.rs             INT 09h (キーボード)
      timer.rs                INT 1Ch (タイマ)
      system.rs               INT 11h/12h/1Ah (機器構成/メモリ/時刻)
    io/
      mod.rs                  I/Oポートエミュレーション
      pic.rs                  PIC仮想化
      pit.rs                  PIT仮想化
    loader.rs                 KERNEL.SYS ローダー
```

### FFIインターフェース

```rust
/* Cから呼ばれるエントリポイント */
#[no_mangle]
pub extern "C" fn v86_dispatch(
    regs: *mut V86Regs,      /* V86レジスタ (pushad + セグメント) */
    fault_eip: u32,          /* フォルト発生EIP */
    fault_cs: u16,           /* フォルト発生CS */
) -> V86Action {
    /* ... 命令デコード → 処理 → 復帰方法を返す */
}

/* OS32カーネル関数へのFFI (extern "C") */
extern "C" {
    fn vfs_read(path: *const u8, buf: *mut u8, size: u32) -> i32;
    fn vfs_write(path: *const u8, buf: *const u8, size: u32) -> i32;
    fn kbd_get_scancode() -> u8;
    /* ... */
}
```

---

## 3. 既存コード変更

### [gdt.c](file:///mnt/c/WATCOM/src/os32/kernel/gdt.c)

```c
/* 3エントリ → 5エントリに拡張 */
struct gdt_entry gdt[5];  /* NULL, Code, Data, TSS low, TSS high */
```

### [isr_handlers.c](file:///mnt/c/WATCOM/src/os32/kernel/isr_handlers.c)

`exception_handler()` にV86モード判定を追加。
EFLAGS.VM ビットが立っていたら Rust の `v86_dispatch()` を呼ぶ。

### [isr_stub.asm](file:///mnt/c/WATCOM/src/os32/kernel/isr_stub.asm)

V86モードからの#GP時のスタックフレーム差異に対応する専用スタブ追加。

### [paging.c](file:///mnt/c/WATCOM/src/os32/kernel/paging.c)

`paging_create_v86_pd()` を追加。

### build/kernel.mk

os32_v86 クレートのビルドとリンクを追加。

---

## 4. 新規ファイル

| ファイル | 言語 | 内容 |
|---------|------|------|
| `kernel/tss.c` | C | TSS構造体初期化、ESP0/SS0、I/Oビットマップ |
| `kernel/tss.h` | C | TSS定義 |
| `kernel/v86_entry.asm` | NASM | V86モード遷移 (IRET) / 復帰コード |
| `kernel/v86/` | Rust | os32_v86 クレート (上記構成) |
| `exec/exec_dos.c` | C | `dos` コマンド実装 (V86起動の薄いラッパー) |

---

## 5. I/Oポート方針

| ポート | デバイス | ビット | 理由 |
|--------|---------|--------|------|
| 7CH, 7EH | GRCG | 0 (許可) | ネイティブ速度 |
| A0H-AEH | グラフィックGDC・パレット | 0 (許可) | ネイティブ速度 |
| A4H, A6H | 表示/描画ページ | 0 (許可) | ネイティブ速度 |
| 60H-6AH | テキストGDC・モードFF | 0 (許可) | VZ Editorのテキスト表示に必要 |
| 04A0H-04AEH | EGC | 0 (許可) | ネイティブ速度 |
| 188H-18EH | FM音源 | 0 (許可) | ネイティブ速度 |
| 41H, 43H | キーボード8251 | 0 (許可) | VZ Editorのキー入力に必要 |
| 00H, 02H | マスタPIC | 1 (トラップ) | 割り込み管理はOS32が制御 |
| 08H, 0AH | スレーブPIC | 1 (トラップ) | 同上 |
| 71H, 77H | PIT | 1 (トラップ) | タイマ管理はOS32が制御 |

---

## 6. VZ Editor 対応で必要な要件

VZ Editor はPC-98用の定番テキストエディタ。動作要件:

| 要件 | VDMでの対応 |
|------|-----------|
| テキストVRAM直接書き込み | ✅ 実機VRAMに直接マッピング |
| キーボード割り込み (INT 09h) | INT 09h のBIOS INTエミュレーション |
| キーボード8251直接ポーリング | ✅ I/Oスルー |
| INT 21h ファイルI/O | ✅ FreeDOS(98)が処理 |
| INT 21h メモリ管理 | ✅ FreeDOS(98)が処理 |
| テキストVRAM属性操作 | ✅ 実機VRAMに直接マッピング |
| テキストGDC (カーソル制御) | ✅ I/Oスルー |
| 常駐 (TSR) | FreeDOS(98)のINT処理に依存 |

> [!TIP]
> VZ Editorは主にテキストVRAM + キーボードで動作するCUIアプリ。
> グラフィックVRAMやGRCGは使わない。VDMの初期段階で動作する可能性が高い。

---

## 7. DOS→OS32復帰メカニズム

COMMAND.COMの `EXIT` 時にFreeDOSカーネルがリブートを試みるタイミングで、
OS32のV86モニタが検知し `longjmp` でOS32シェルに復帰する。

復帰時の処理:
1. GRCG OFF (ポート 7CH に 0x00)
2. EGC OFF (6AH に 0x04, 0x06)
3. 16色モード復帰 (6AH に 0x01)
4. パレットをOS32デフォルトにリセット
5. テキストVRAM / GDCをOS32コンソール用に再初期化
6. V86用ページディレクトリ破棄、マスターPDに復帰
7. OS32シェルの画面を再描画

---

## 8. 段階的ロードマップ

### Phase 0: TSS + V86基盤 — 2-3日

**C/NASM:**
1. `kernel/tss.c` / `tss.h` — TSS構造体定義と初期化
2. `kernel/gdt.c` — GDTにTSSディスクリプタ追加、`ltr` でロード
3. `kernel/v86_entry.asm` — IRET による V86モードへの遷移
4. `kernel/isr_stub.asm` — V86用 #GP スタブ

**Rust:**
5. `kernel/v86/` — os32_v86 クレート骨格
6. `v86/src/decoder.rs` — 基本命令デコーダ (INT/CLI/STI/PUSHF/POPF/HLT)

**検証**: V86モードで HLT → #GP トラップ → Rust デコーダで処理 → 復帰

### Phase 1: V86メモリ空間 + 最小テスト — 1週間

**C:**
1. `kernel/paging.c` — `paging_create_v86_pd()` 実装

**Rust:**
2. `v86/src/context.rs` — V86コンテキスト (IVT, BDA)
3. `v86/src/dispatch.rs` — V86内INT → IVT転送メカニズム
4. `v86/src/bios/system.rs` — INT 11h/12h 最小実装
5. `v86/src/bios/video.rs` — INT 18h 文字出力 最小実装
6. `v86/src/loader.rs` — COMファイルローダー

**検証**: 自作 `HELLO.COM` がテキストVRAMに文字表示

### Phase 2: FreeDOS(98) + コマンドプロンプト — 2-3週間

**C:**
1. `exec/exec_dos.c` — `dos` コマンド実装

**Rust:**
2. `v86/src/loader.rs` — KERNEL.SYS ローダー
3. `v86/src/bios/disk.rs` — INT 1Bh → VFS ブリッジ
4. `v86/src/bios/video.rs` — INT 18h テキスト操作拡充
5. `v86/src/bios/keyboard.rs` — INT 09h キーボード
6. `v86/src/bios/timer.rs` — INT 1Ch タイマ
7. `v86/src/io/pic.rs` — PIC仮想化
8. `v86/src/io/pit.rs` — PIT仮想化
9. DOS→OS32復帰メカニズム

**検証**:
- FreeDOS(98) プロンプト起動
- `dir`, `type`, `copy` 動作
- `exit` でOS32シェル復帰
- 復帰後のOS32正常動作

### Phase 3: VZ Editor 動作 — 2-3週間

**Rust:**
1. `v86/src/bios/keyboard.rs` — キーボードBIOS拡充 (スキャンコード変換)
2. `v86/src/bios/video.rs` — テキストBIOS拡充 (カーソル制御等)
3. INT 1Bh ディスクBIOS拡充 (VZ Editorのファイル操作)
4. 追加BIOS INTの実装 (必要に応じて)
5. 復帰時画面リストア処理

**検証**:
- VZ Editorが起動しテキスト表示される
- キー入力でカーソル移動・編集が動作
- ファイルの読み込み・保存が動作
- VZ Editor終了後 `exit` でOS32に正常復帰

---

## 9. ビルド環境

| ツール | 用途 | 環境 |
|--------|------|------|
| OpenWatcom | FreeDOS(98) KERNEL.SYS ビルド | Windows |
| NASM | FreeDOS(98) ASM + OS32 ASMスタブ | Windows / WSL |
| UPX | KERNEL.SYS 圧縮 (任意) | Windows |
| i386-elf-gcc | OS32カーネル C部分 | WSL |
| Rust (i686-os32-none) | os32_v86 クレート | WSL |
