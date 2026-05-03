# VDM (Virtual DOS Machine) — 実装計画書 v5

OS32シェルから `dos` コマンドで FreeDOS(98) を V86モードで起動し、
DOSコマンドプロンプトを提供する。`exit` でOS32シェルに復帰する。
テスト目標: **VZ Editor の起動**。

---

## 1. 進捗状況

| Phase | 概要 | 状態 |
|-------|------|------|
| Phase 0 | TSS + V86基盤 + カーネル統合テスト | ✅ 完了 |
| Phase 1 | V86メモリ空間 + 最小COMファイル実行 | ✅ 完了 |
| Phase 2 | FreeDOS(98) + コマンドプロンプト | 🔄 進行中 (IPL通過→カーネルハング→デバッグ環境構築完了) |
| Phase 3 | VZ Editor 動作 | 未着手 |

### Phase 0 完了事項

- `kernel/tss.c` — TSS構造体、ESP0/SS0切り替え、I/Oビットマップ(全ポートトラップ)
- `kernel/gdt.c` — GDTにTSSディスクリプタ追加、`ltr` でロード
- `kernel/v86_entry.asm` — IRET によるV86モード遷移
- `kernel/isr_stub.asm` — V86用 #GP/#PF スタブ + **DS/ES復元** (必須)
- `kernel/v86.c` — #GPハンドラ (CLI/STI/HLT/INT/PUSHF/POPF/IRET/IN/OUT)
- `kernel/v86_test.c` — カーネル統合V86動作検証テスト
- `kernel/paging.c` — `paging_pde_set_flags()` / `paging_pde_clear_flags()`

検証結果: V86モード遷移 → TVRAM書き込み ("V86 OK!") → HLT → #GPトラップ →
longjmpによるカーネル復帰 → ブートスプラッシュ表示まで正常動作。

> [!NOTE]
> Phase 0で判明した重要な教訓:
> V86→Ring0遷移時にCPUはDS/ES/FS/GSを**0にクリア**する。
> #GPハンドラ (isr_stub.asm) の先頭でDS=0x10, ES=0x10を即座に復元しないと
> Cハンドラ内の全メモリアクセスが不正になりトリプルフォルトする。

---

## 2. アーキテクチャ概要

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

## 3. メモリレイアウト

### 物理メモリ配置

```
0x000000-0x09FFFF : コンベンショナルメモリ (実機ハードウェア)
0x0A0000-0x0FFFFF : VRAM + ROM (実機ハードウェア)
0x100000-0x1FFFFF : OS32カーネル帯域 (1MB)
0x200000-0x2FFFFF : SQLite帯域 (1MB)
0x300000-0x39FFFF : V86バッキングRAM (640KB, DOSメインメモリ)
0x400000〜        : 未使用 (将来拡張用)
```

### V86タスクのページング (仮想→物理マッピング)

出典: PC9800Bible §4-2 メモリマップ, §2-6 テキスト, §2-7 グラフィック

```
仮想アドレス       物理アドレス        用途                  属性
─────────────────────────────────────────────────────────────────
0x00000-0x003FF    0x300000-0x3003FF   IVT (割り込みベクタ)   R/W, USER
0x00400-0x005FF    0x300400-0x3005FF   BDA (BIOSデータエリア) R/W, USER
0x00600-0x9FFFF    0x300600-0x39FFFF   FreeDOS + ユーザ領域   R/W, USER
0xA0000-0xA1FFF    0xA0000 (実機)      TVRAM 文字エリア       R/W, USER
0xA2000-0xA3FFF    0xA2000 (実機)      TVRAM 属性エリア       R/W, USER
0xA4000-0xA7FFF    0xA4000 (実機)      CGウィンドウ (VX以降)  R/O, USER
0xA8000-0xAFFFF    0xA8000 (実機)      GVRAM Plane0 (Blue)    R/W, USER
0xB0000-0xB7FFF    0xB0000 (実機)      GVRAM Plane1 (Red)     R/W, USER
0xB8000-0xBFFFF    0xB8000 (実機)      GVRAM Plane2 (Green)   R/W, USER
0xC0000-0xDFFFF    NOT PRESENT         未使用 (トラップ)      —
0xE0000-0xE7FFF    0xE0000 (実機)      GVRAM Plane3 (輝度)    R/W, USER
0xE8000-0xEFFFF    NOT PRESENT         バンクメモリ (トラップ) —
0xF0000-0xFFFFF    0xF0000 (実機)      BIOS ROM               R/O, USER
```

### VRAM方針

DOSモード中はVRAM + GRCG/EGC を**完全にDOSに明け渡す**。
- VRAM領域は実機ハードウェアに直接マッピング (バックバッファ不要)
- GRCG/EGC の I/Oポートもスルー (I/Oビットマップで許可)
- DOS→OS32復帰時にGRCGを確実にOFF + 画面状態をリストア

---

## 4. I/Oポートビットマップ

出典: PC9800Bible §4-3 I/Oマップ, UNDOCUMENTED io_pic.md / io_tcu.md

### 許可ポート (ビット=0, DOSが直接アクセス)

| ポート範囲 | デバイス | 根拠 |
|-----------|---------|------|
| `41h, 43h` | キーボード 8251 | Bible §4-3 #9 |
| `60h-6Ah` (偶数) | テキストGDC + モードFF | Bible §2-6 表2-22 |
| `70h-7Eh` (偶数) | CRTC + GRCG | Bible §4-3 #12 |
| `A0h-AEh` (偶数) | グラフィックGDC + パレット | Bible §2-7 表2-30 |
| `A4h, A6h` | 表示/描画ページ切替 | Bible §2-7-3 |
| `04A0h-04AEh` (偶数) | EGC | Bible §4-3 #20 |
| `0188h-018Eh` (偶数) | FM音源 YM2203/2608 | Bible §4-3 #15 |

### トラップポート (ビット=1, OS32がエミュレーション)

| ポート | デバイス | 処理内容 | 根拠 |
|--------|---------|---------|------|
| `00h, 02h` | マスタPIC 8259A | EOI/IMR仮想化 | Bible §4-3 #1, UNDOC io_pic.md |
| `08h, 0Ah` | スレーブPIC 8259A | 同上 | Bible §4-3 #2, UNDOC io_pic.md |
| `35h` | システムポート ポートC | BUZ/SHUT制御 | Bible §4-3 #7 |
| `37h` | システムポート 8255CMD | ブザーON/OFF | Bible §4-3 #7 |
| `71h` | PIT Counter#0 | タイマ値R/W | UNDOC io_tcu.md |
| `73h, 3FDBh` | PIT Counter#1 | ビープ音周波数 | UNDOC io_tcu.md |
| `77h, 3FDFh` | PIT コマンド | モード設定 | UNDOC io_tcu.md |
| `F0h` | CPUリセット | リブート検知→V86終了 | Bible §4-3 #31 |

> [!IMPORTANT]
> **PC-98のPICポートアドレスはPC/ATと異なる**:
> PC-98: マスタ=`00h/02h`, スレーブ=`08h/0Ah`
> PC/AT: マスタ=`20h/21h`, スレーブ=`A0h/A1h`
> EOI発行: `OUT 00h, 20h` (マスタ), `OUT 08h, 20h` (スレーブ)

---

## 5. BIOS INT と割り込みベクタ

出典: PC9800Bible §4-5 割り込みベクタ, §2-6 テキストBIOS, §4-7 MS-DOS

### PC-98 ハードウェア割り込み (PIC経由)

| INT | 用途 | VDM処理 | Phase |
|-----|------|---------|-------|
| `08h` | タイマ (PIT Counter#0, 100Hz) | V86に仮想割り込みリフレクト | 2 |
| `09h` | キーボード | V86にリフレクト | 2 |
| `0Ah` | V-SYNC (CRTV) | V86にリフレクト | 3 |
| `0Bh-0Eh` | 拡張バス/RS-232C | 不要 | — |
| `0Fh` | スレーブPIC接続 | 不要 | — |
| `10h-17h` | スレーブ (NDP/HDD/FDD/FM/マウス等) | 不要 (VZ EditorはCUI) | — |

### BIOS / DOS ソフトウェア割り込み

| INT | 用途 | VDM処理 | Phase |
|-----|------|---------|-------|
| `11h` | 機器構成取得 | BDA応答 | 2 |
| `12h` | メモリサイズ取得 | 固定値応答 | 2 |
| `18h` | キーボード/CRT BIOS | IVT→FreeDOS or 最小エミュレーション | 1-3 |
| `19h` | RS-232C BIOS | リブート検知 | 2 |
| `1Ah` | プリンタ BIOS | 不要 | — |
| `1Bh` | ディスク BIOS | VFS ブリッジ | 2 |
| `1Ch` | カレンダ/タイマ BIOS | 仮想カレンダ応答 | 2 |
| `21h` | DOS ファンクションコール | IVT→FreeDOS(98)カーネルに転送 | 2 |

---

## 6. BDA (BIOS Data Area) 初期構築

出典: UNDOCUMENTED memsys.md

FreeDOS(98) の起動に最低限必要なシステム共通域の初期値:

| アドレス | サイズ | 名前 | 設定値 | 用途 |
|---------|--------|------|--------|------|
| `0000:0400h` | 1B | BIOS_FLAG2 | `00h` | 機種フラグ |
| `0000:0401h` | 1B | EXPMMSZ | `00h` | 拡張メモリサイズ (PM未使用) |
| `0000:0480h` | 1B | CPU_FLAG | `08h` | CPU種別 (bit3=V33A=0) |
| `0000:0484h` | 4B | CPU_TYPE | `03h, 00h, ...` | i386以上 |
| `0000:0495h` | 1B | GRAPH_CHG | `00h` | GRCGモード (OFF) |
| `0000:0496-0499h` | 4B | GRAPH_TAL | `00 00 00 00` | GRCGタイルレジスタ |
| `0000:0501h` | 1B | BIOS_FLAG5 | `00h` | bit7=CLK(10MHz系), bit2-0=RAM |
| `0000:055Ch` | 1B | DISK_EQUIP | ※ | FDD/HDDドライブ接続情報 |

---

## 7. DOS→OS32 復帰メカニズム

FreeDOS(98) COMMAND.COM の `EXIT` 時、DOS カーネルがリブートを試みるタイミングで
OS32のV86モニタが検知し `longjmp` でOS32シェルに復帰する。

### 検知方法 (優先度順)

1. `OUT F0h` (CPUリセットポート) → I/Oビットマップでトラップ → V86終了
2. `JMP FFFF:0000` → リニアアドレス 0xFFFF0 へのジャンプ → IVT経由で検知
3. `INT 19h` → IVTに独自ハンドラ設置 → V86終了

### 復帰時の処理

1. GRCG OFF (ポート `7Ch` に `00h`)
2. EGC OFF (`6Ah` に `04h`, `06h`)
3. 16色モード復帰 (`6Ah` に `01h`)
4. パレットをOS32デフォルトにリセット
5. テキストVRAM / GDCをOS32コンソール用に再初期化
6. V86用ページテーブル破棄、マスターPDに復帰
7. TSS ESP0をカーネルスタックに戻す
8. OS32シェルの画面を再描画

---

## 8. 段階的ロードマップ

### Phase 1: V86メモリ空間 + 最小COMファイル実行 — 完了

1. ✅ `paging_create_v86_pd()` — §3 ページマップの完全実装
2. ✅ BDA初期構築 (§6 の最低限の値を設定)
3. ✅ IVT初期構築 (INT 00h-1Fh にダミーIRETハンドラ設置)
4. ✅ `INT` 命令の完全エミュレーション (IVT参照 → V86内ハンドラ呼出)
5. ✅ COMファイルローダー
6. ✅ INT 18h AH=0Ah (テキスト画面モード設定) 最小実装
7. ✅ INT 18h AH=13h (カーソル位置設定) 最小実装
8. ✅ **検証**: 自作 `HELLO.COM` がTVRAMに直接書き込みで文字表示

### Phase 2: FreeDOS(98) ブート — 進行中

1. ✅ FreeDOS(98) KERNEL.SYS ローダー (VFS経由 FDDイメージ読み込み)
2. ✅ INT 1Bh ディスクBIOS → VFS ブリッジ (AH=01h/03h/04h/05h/06h/07h/46h/56h/84h)
3. ✅ INT 08h タイマ割り込みリフレクト (100Hz, ISR/IMRガード付き)
4. INT 09h キーボード割り込みリフレクト
5. ✅ PIC仮想化 (EOI/IMR/IRR/ISR エミュレーション)
6. ✅ PIT仮想化 (Counter#0/1/2 モード設定・読み出し)
7. ✅ INT 1Ch カレンダBIOS (日時取得)
8. ✅ INT 11h (機器構成取得) / INT 12h (メモリサイズ)
9. ✅ `OUT F0h` リブート検知 → DOS→OS32復帰
10. ✅ GRCG OFF + パレットリセット + 画面リストア
11. ✅ fdkernel ソースビルド環境構築 (OpenWatcom + WSLハイブリッド)
12. ✅ BDA/メモリスイッチ追加初期化 (0x0458/0x0482/0x055D/0x03FE/0x05AE + TVRAM MEMSW)
13. ✅ INT 18h BIOS拡充 (AH=0Eh/14h/15h/17h/1Ah/1Bh/40h/41h/42h/43h)
14. ✅ V86タイムアウトベース制御 (10秒)
15. 🔄 カーネル初期化ハング箇所特定 → デバッグ中
16. **検証**: FreeDOS(98) プロンプト起動、`dir`/`type`/`exit` 動作

> [!NOTE]
> **Phase 2 現在地**: IPL正常動作 → kernel.sys正常ロード → カーネル初期化実行中。
> タイムアウトをカウントベースからtick_countベース(10秒)に変更し、
> BDA追加初期化 + INT 18h BIOS拡充を実施。テスト待ち。
> 詳細: [freedos98_boot_debug_report.md](freedos98_boot_debug_report.md)

### Phase 3: VZ Editor 動作 — 2〜3週間

1. INT 18h テキストBIOS拡充 (AH=0Ch/0Dh/10h/11h/12h/14h/16h)
2. INT 09h キーボードBIOS拡充 (スキャンコード→BDA変換)
3. ビープ音エミュレーション (PIT Counter#1 + System Port BUZ)
4. VSYNC割り込み (INT 0Ah) リフレクト
5. INT 1Bh ディスクBIOS拡充 (ファイル読み書き)
6. 追加テキストGDC操作 (カーソル形状、30行モード等)
7. **検証**: VZ Editor起動、テキスト編集、ファイル保存、終了

---

## 9. VZ Editor 対応で必要な要件

出典: PC9800Bible §2-6 テキストBIOS一覧 (INT 18h)

VZ Editor はPC-98用の定番CUIテキストエディタ。動作要件:

| 要件 | VDMでの対応 |
|------|-----------| 
| テキストVRAM直接書き込み (A0000-A3FFF) | ✅ 実機VRAMに直接マッピング |
| アトリビュート操作 (色/反転/下線) | ✅ 実機VRAMに直接マッピング |
| キーボード8251直接ポーリング (41h/43h) | ✅ I/Oスルー |
| キーボード割り込み (INT 09h) | V86にリフレクト |
| テキストGDC カーソル制御 (60h/62h) | ✅ I/Oスルー |
| INT 21h ファイルI/O | ✅ FreeDOS(98)が処理 |
| INT 21h メモリ管理 | ✅ FreeDOS(98)が処理 |
| 常駐 (TSR) | FreeDOS(98)のINT処理に依存 |

---

## 10. 関連ファイル

### カーネルソース

| ファイル | 説明 |
|----------|------|
| [v86.c](file:///mnt/c/WATCOM/src/os32/kernel/v86.c) | V86 #GPハンドラ (命令エミュレータ) |
| [v86.h](file:///mnt/c/WATCOM/src/os32/kernel/v86.h) | V86コンテキスト定義 |
| [v86_mem.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_mem.c) | V86メモリ構築、BDA/IVT初期化 |
| [v86_bios.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_bios.c) | BIOS割り込みエミュレータ (INT 18h, 29h, 1Ch等) |
| [v86_disk.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_disk.c) | INT 1Bh ディスクBIOS (FDD 2HD) |
| [v86_disk.h](file:///mnt/c/WATCOM/src/os32/kernel/v86_disk.h) | ディスクBIOSヘッダ |
| [v86_pic.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_pic.c) | V86向けPIC仮想化ロジック |
| [v86_pit.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_pit.c) | PIT仮想化 (Counter#0/1/2) |
| [v86_test.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_test.c) | カーネル統合V86テスト |
| [v86_entry.asm](file:///mnt/c/WATCOM/src/os32/kernel/v86_entry.asm) | IRET によるV86モード遷移 |
| [isr_stub.asm](file:///mnt/c/WATCOM/src/os32/kernel/isr_stub.asm) | #GP/#PFスタブ (V86 DS/ES復元含む)、全IRQスタブDS/ES対応 |
| [tss.c](file:///mnt/c/WATCOM/src/os32/kernel/tss.c) | TSS構造体・I/Oビットマップ |
| [paging.c](file:///mnt/c/WATCOM/src/os32/kernel/paging.c) | PDE/PTEフラグ操作 |

### FreeDOS(98) ビルド環境

| ファイル | 説明 |
|----------|------|
| [build98.sh](file:///mnt/c/WATCOM/src/os32/tools/fdkernel/nec98/build98.sh) | ハイブリッドビルドスクリプト (WSL+cmd.exe) |
| [fdkernel_build.md](fdkernel_build.md) | ビルド手順書 |
| `tools/fdkernel/nec98/bin/kernel.sys` | カスタムビルド版カーネル (DEBUG) |
| `tools/freedos98/fd98_2hd.img` | FreeDOS(98) 2HD FDDイメージ (1.2MB) |

### PoC / テスト

| ファイル | 説明 |
|----------|------|
| [v86_fdd_test.asm](file:///mnt/c/WATCOM/src/os32/tests/v86_fdd_test.asm) | FDDブート用V86 PoCテスト (Phase 0参考) |

### 参考ドキュメント

| ドキュメント | 内容 |
|-------------|------|
| [PC9800Bible §4-3](file:///mnt/c/WATCOM/docs/PC9800Bible/4-3_I_Oマップ.md) | I/Oポートマップ (全32デバイス) |
| [PC9800Bible §4-5](file:///mnt/c/WATCOM/docs/PC9800Bible/4-5_割り込みベクタ.md) | 割り込みベクタ一覧 |
| [PC9800Bible §2-6](file:///mnt/c/WATCOM/docs/PC9800Bible/2-6_テキスト.md) | テキストVRAM / GDC / BIOS |
| [PC9800Bible §2-7](file:///mnt/c/WATCOM/docs/PC9800Bible/2-7_グラフィック.md) | グラフィックVRAM / パレット |
| [PC9800Bible §4-7](file:///mnt/c/WATCOM/docs/PC9800Bible/4-7_MS-DOS.md) | DOSファンクションコール一覧 |
| [UNDOC io_pic.md](file:///mnt/c/WATCOM/docs/undocumented/io_pic.md) | PIC仕様 (EOI手順含む) |
| [UNDOC io_tcu.md](file:///mnt/c/WATCOM/docs/undocumented/io_tcu.md) | PIT仕様 (クロック周波数) |
| [UNDOC memsys.md](file:///mnt/c/WATCOM/docs/undocumented/memsys.md) | BIOSデータエリア全定義 |
