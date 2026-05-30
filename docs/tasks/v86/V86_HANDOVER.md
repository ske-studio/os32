# V86サブシステム 現状報告と引き継ぎ資料

最終更新: 2026-05-17
ブランチ: `feat/vdm` (HEAD: `1e505fe`)

---

## 1. V86サブシステム概要

OS32カーネル上で16bit DOSプログラム (FreeDOS(98) / COMファイル) を実行するための
仮想8086モード (Virtual 8086 Mode) サブシステム。

```
[OS32 カーネル (32bit PM)]
  ├── v86_session.c  — セッション管理 (ブート/終了/タイムアウト)
  ├── v86.c          — #GPハンドラ (命令エミュレーション + IRQ注入)
  ├── v86_mem.c      — 1MBバッキングRAM + ページテーブル管理
  ├── v86_iocore.c   — I/Oポートディスパッチ (仮想化テーブル)
  │    ├── v86_pic.c  — PIC 8259A 仮想化
  │    ├── v86_pit.c  — PIT 8253A 仮想化
  │    ├── v86_dma.c  — DMA 8237A 全4ch仮想化
  │    ├── v86_fdc.c  — FDC µPD765A ステートマシン
  │    └── v86_vsync.c — VSYNC (INT 0Ah) 注入
  ├── v86_bios.c     — INT 18h (CRT/KBD BIOS) HLE
  ├── v86_disk.c     — INT 1Bh (ディスクBIOS) HLE
  └── v86_debug.c    — 診断ログ (v86_diag.log)
```

**総行数**: 約12,300行 (36ファイル)

---

## 2. 現在の動作状況

### 動くもの ✅

| 機能 | 状態 | 備考 |
|------|------|------|
| COMファイル実行 | ✅ | 単純なDOS COMは正常実行 |
| IPLロード (FDI) | ✅ | FreeDOS(98) `dos5_1.fdi` のIPLセクタ読み込み成功 |
| INT 1Bh (ディスクBIOS) | ✅ | SEEK/READ/WRITE/VERIFY/SENSE/READ ID/RECALIBRATE/INITIALIZE/密度設定 |
| INT 18h (CRT BIOS) | ✅ | KBD入力/CRT表示/VRAMクリア/スクロール/パレット |
| INT 29h (高速1文字出力) | ✅ | HLE維持 |
| FDC ポートレベル仮想化 | ✅ | 0x90/0x92/0x94/0xBE 全対応 |
| PIC 完全仮想化 | ✅ | ICW1-4/OCW2(ローテーション付)/OCW3(SMM) NP21/W準拠 |
| DMA 全4ch仮想化 | ✅ | BIOS ROM初期化によるOS32 DMA破壊を防止 |
| IRQ0 (タイマ) 注入 | ✅ | GP末尾 + タイマIRQハンドラ双方から注入 |
| IRQ1 (KBD) 注入 | ✅ | BDA直書き方式 |
| IRQ2 (VSYNC) 注入 | ✅ | ワンショット + レート制御 |
| 診断ログ (F12終了時) | ✅ | BDA/PIC/FDC/DMA/I/O統計/EOI/INT頻度 |
| OS32カーネル生存 | ✅ | V86クラッシュ後もカーネルは正常動作 |

### 動かないもの ❌

| 機能 | 状態 | 問題 |
|------|------|------|
| **FreeDOS(98) ブート完走** | ❌ | IPL→IO.SYS読み込み途中で `#UD` クラッシュ |
| **BIOS ROM INT 08h 注入** | ❌ | ROM内ハンドラへの直接注入はトリプルフォルト |

---

## 3. 最大のブロッカー: `#UD (EIP=0x0F)` クラッシュ

### 症状

FreeDOS `dos5_1.fdi` をV86で起動すると、IPLがIO.SYSを読み込む途中で
カーネルのクラッシュスクリーンが表示される。

```
Vec: 0x00000006   ErrC: 0x00000000
EIP: 0x0000000F  [OUT OF CODE!]
EAX=00EF1306  EBX=000004D9  ECX=00000007
EDX=00000220  ESI=0014FFF1  EDI=00002221
EBP=00EF0000  ESP=0016AA64
```

### 分析

- **Vec 0x06 = #UD (Invalid Opcode)**: 存在しない命令を実行しようとした
- **EIP=0x0F**: カーネルコード領域外 (カーネルは `0x110000` 以降にロード)
- **EBP=0x00EF0000**: 明らかに異常な値 (通常のカーネルスタックは `0x0016xxxx`)

### 根本原因 (推定)

V86モードからカーネルモード (#GP例外) に復帰する際、**スタックフレームまたはコンテキストが破壊**されている。

過去の調査で特定された要因:
1. **v86_context のスタック配置問題** (Conversation `3a8fc9bc`): v86_context構造体がカーネルスタック上に配置され、長時間実行でスタック溢れにより破壊される。`static`化で一時的に緩和されたが根治ではない。
2. **BIOS ROM実行によるコンテキスト干渉** (Commit `28f998f`): BIOS ROM内のコードがV86モードで実行される際、ROM内の特定命令シーケンスがGPハンドラの想定外の動作を引き起こす。
3. **IRQ注入タイミング**: タイマIRQハンドラがV86スタックフレームを書き換えている最中に#GP例外が発生すると、レジスタ復帰が破綻する可能性。

### 過去の対策と現状

| 対策 | 状態 | 効果 |
|------|------|------|
| v86_context を static化 | ✅ 適用済 | スタック溢れの緩和。根治ではない |
| BIOS ROM実行のハイブリッド方式 | ✅ 適用済 | ROM委譲 + I/O仮想化。ROM実行自体は許可 |
| Layer 1+2 ROM行きIVTインターセプト | ✅ 適用済 | ダミー/ROM IVTハンドラへの注入回避 |
| GPハンドラ内kprintf排除 | ✅ 適用済 | 再帰ハング防止 |
| HLE全面強化 (NP21/W準拠) | ✅ 適用済 | 本セッションの成果 |
| **V86 entry/exit のasm検証** | ❌ 未着手 | **最も疑わしいが未調査** |
| **TSS ESP0 の整合性検証** | ❌ 未着手 | V86→PM復帰時のスタックポインタ |

---

## 4. アーキテクチャ方針

### ハイブリッド方式 (現行)

```
INT 1Bh (ディスク)  → HLE (v86_disk.c)    — ROM非経由
INT 29h (高速出力)  → HLE (v86_bios.c)    — ROM非経由
INT 18h/1Ch/11h/12h → IVT転送 → ROM実行  — ROM内I/OはGP仮想化
INT 08h (タイマ)    → HLT時BDA直接更新 / ゲストハンドラ注入
```

- **HLE維持**: 高頻度/ディスクI/O系はHLEで直接処理 (GPオーバーヘッド回避)
- **ROM委譲**: 低頻度BIOS系はROMに委譲 (ROM内のI/O命令はGPで仮想化)

### I/Oポート仮想化

v86_iocore.cのディスパッチテーブルで分類:

| 分類 | 方式 | 例 |
|------|------|-----|
| 仮想 | 仮想レジスタ操作 | PIC/PIT/DMA/FDC/KBD/VSYNC |
| ブロック | アクセス破棄 | RS-232C/HostDrv/リセット |
| パススルー | 実HWアクセス | ブザー/GDC PRAM |
| 未登録 | 実HWフォールスルー | 一部GDC/サウンド系 |

---

## 5. NP21/W HLE準拠状況

[`v86_hle_full_comparison.md`](file:///mnt/c/WATCOM/src/os32/docs/tasks/v86/v86_hle_full_comparison.md) に全モジュールの詳細比較あり。

| モジュール | 準拠度 | 備考 |
|-----------|--------|------|
| PIC 8259A | **95%** | ICW値保存のみ未実装。OCW2/OCW3 NP21/W完全準拠 |
| PIT 8253A | **70%** | tick推定方式。クロック精度・BCD・リードバック未対応 |
| DMA 8237A | **90%** | 全4ch仮想化済み。TC/動的チャネル管理は不要 (HLE方式) |
| FDC µPD765A | **85%** | 主要コマンド全対応。SCAN系/エラーシミュレーション未対応 |
| INT 18h | **80%** | KBD/CRT基本対応。GDC完全初期化/CG RAM未対応 |
| INT 1Bh | **90%** | READ/WRITE/SEEK/SENSE/VERIFY/INITIALIZE/密度/READ ID 全対応 |
| BDA/メモリ | **100%** | NP21/W準拠。1MBバッキング+A20 PTE |

---

## 6. 未実装・将来課題

### P2: 機能補完 (DOSブートに不要)

| タスク | ファイル | 説明 |
|--------|---------|------|
| INT 18h AH=1Bh スクロールダウン | v86_bios.c | 現在NOP |
| FDC FORMAT TRACK (0Dh) | v86_disk.c | FORMAT.COM用 |
| PIT リードバック (SC=11) | v86_pit.c | ゲーム用 |
| PIT クロック精度 | v86_pit.c | ゲーム用 |

### 構造的課題 (クラッシュ解消に必要)

| タスク | 説明 |
|--------|------|
| **V86 entry/exit asm 検証** | `v86_entry.asm` のレジスタ保存・復帰順序の正当性確認 |
| **TSS ESP0 整合性** | V86→PM遷移時にTSS.ESP0が正しいカーネルスタックを指しているか |
| **IRQハンドラとGPの競合** | タイマIRQ (irq_timer) がV86スタックフレームを書き換える際の排他制御 |
| **v86_context永続化** | static変数ではなくkmalloc確保された専用領域への移行 |
| **#PF (Page Fault) ハンドラ** | V86 1MB空間外のアクセスでの安全な処理 |

---

## 7. デバッグ環境

### ツール

| ツール | 場所 | 用途 |
|--------|------|------|
| os32_server.py | tools/os32_server.py | HTTP API経由のリモート操作 |
| NP21/W スクリーンショット | `/screenshot` API | 画面キャプチャ |
| v86_diag.log | F12終了時に `/debug/` に出力 | BDA/PIC/FDC/DMA/I/O統計 |
| シリアルログ | `/os32_serial_log.txt` | カーネルシリアル出力 |

### ワークフロー

```bash
# ビルド → デプロイ → NP21/W再起動 → テスト
/build-os32   # 全自動ワークフロー

# プログラムのみ (NP21/W再起動不要)
/deploy-program

# V86デバッグセッション
/debug-v86

# V86ログ後分析
/analyze-v86-logs
```

### テスト用FDI

| ファイル | 内容 |
|---------|------|
| `/dos5_1.fdi` | FreeDOS(98) 1.44MB 2HD ブートFD |

---

## 8. 関連ドキュメント

| ドキュメント | パス | 内容 |
|-------------|------|------|
| V86アーキテクチャ | `docs/tasks/v86/01_architecture.md` | 全体設計 |
| CPU エミュレーション | `docs/tasks/v86/02_cpu_emulation.md` | GP命令一覧 |
| I/Oポートマップ | `docs/tasks/v86/03_io_port_map.md` | ポート別仮想化方式 |
| メモリマップ | `docs/tasks/v86/04_memory.md` | バッキングRAM/PTE |
| IRQ注入 | `docs/tasks/v86/05_irq_injection.md` | IRQ0/1/2注入方式 |
| BIOS HLE | `docs/tasks/v86/06_bios_emulation.md` | INT 18h/1Bh HLE |
| FDC仮想化 | `docs/tasks/v86/07_fdc_virtualization.md` | FDCステートマシン |
| セッション管理 | `docs/tasks/v86/08_session_lifecycle.md` | ブート/終了 |
| NP21/W比較 | `docs/tasks/v86/v86_hle_full_comparison.md` | **全モジュール詳細比較** |
| HLE実装計画 | `docs/tasks/v86/implementation_plan_hle_full.md` | Phase 1-5 (全完了) |
| BIOSコール比較 | `docs/tasks/v86/bios_hle_comparison.md` | INT 18h/1Bh比較 |

---

## 9. リファレンスソース

| ソース | パス | 用途 |
|--------|------|------|
| **NP21/W** | `src/np21w-src-main/` | HLEの「正解」(Shift-JIS注意) |
| **MS-DOS 4.0** | `src/MS-DOS/v4.0/` | INT 21h/IO.SYS参考 |
| **FreeDOS(98) カーネル** | `src/fdkernel/` | PC-98対応DOS |
| **FreeDOS(98) COMMAND.COM** | `src/freecom_dbcs2/` | DBCS対応シェル |

---

## 10. 次のアクションの推奨

### 短期 (クラッシュ解消)

1. **v86_entry.asm の全面レビュー**: V86モードへの遷移と復帰のレジスタ保存・復帰のasm実装を精査。EIP=0x0Fに飛ぶ原因はここにある可能性が最も高い。

2. **TSS.ESP0 のランタイム検証**: V86セッション中の各#GP発生時にTSS.ESP0が正しいカーネルスタック範囲 (`0x0016xxxx`) を指しているか確認するアサーションを追加。

3. **最小再現**: IPL直後の最初の数命令だけ実行して停止するモード (シングルステップ) で、どの時点でコンテキスト破壊が始まるかを特定。

### 中期 (DOS ブート完走)

4. 上記で特定した原因を修正し、FreeDOS(98)がコマンドプロンプトまで到達することを確認。

5. INT 21h の主要ファンクション (ファイルI/O) のHLE検討 (HostDrv連携)。

### 長期 (ゲーム実行)

6. PIT クロック精度改善。
7. GDC / サウンド I/O のパススルー拡充。
8. ネイティブモードでのゲーム実行安定化。
