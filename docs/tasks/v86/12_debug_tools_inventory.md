# 12. デバッグツール 現状棚卸し

> 既存の V86 デバッグ機構を整理し、不足点 (W1〜W10) を明らかにする。
> 改善設計は [13_debug_tools_design.md](13_debug_tools_design.md)、
> 実装ロードマップは [14_debug_tools_roadmap.md](14_debug_tools_roadmap.md) を参照。

実装本体: [`kernel/v86_debug.c`](../../../kernel/v86_debug.c) (844行)

---

## 12.1 ファイルログ機構

### 起動と出力先

| 項目 | 値 |
|------|-----|
| 起動 | `v86 -d <image>` → `sys_v86_set_debug(1)` → `v86_debug_enabled = 1` |
| 出力先 | `/host/debug/v86_diag.log` (HostDrv 経由) |
| 書込トリガ | V86 セッション終了時に一括書き込み (ヘッダのみ起動時に即時書込) |

### セクション構成

| セクション | 出力内容 |
|----------|---------|
| HEADER | ブートモード / イメージパス / Auto-Typerコマンド |
| EXIT | 終了理由 / Duration / Final CS:IP |
| GP HANDLER | GP数 / INT数 / Last INT / Last CS:IP |
| IRQ0 INJECTION | 8 種類のカウンタ (call/non-vm/no-if/isr-pend/ivt-forward/gp-inject/skip-isr/skip-ivt) |
| VSYNC | arm数 (OUT 64h) / inject数 (INT 0Ah) |
| PIC | IMR/ISR/IRR/EOI count (Master + Slave) |
| PIT | IRQ divisor |
| I/O PORT | 上位16ポートのアクセス回数 |
| DISK I/O | INT 1Bh 直近64件 |
| GP TRACE | 直近128件 (CS:IP/Opcode/INT#/AX/CX) |
| MEMORY DUMP | 0060/0160/1500/A800/0050/BDA/IVT + Timer handler (1024B) + FM Status + PIT |
| TIMEOUT | タイムアウト時 CS:IP + 周辺 opcode 32B |

---

## 12.2 シリアルダンプ

- `v86_debug_dump_serial()` — RS-232C へサマリ出力
- `v86_debug_serial_enabled` フラグ制御 (ファイルログ有効時は自動 OFF)

---

## 12.3 リアルタイム統計カウンタ

[`kernel/v86.h:88-110`](../../../kernel/v86.h) に定義された extern u32 群:

| カウンタ | 用途 |
|---------|------|
| `v86_int_count` | 総 INT 呼び出し回数 |
| `v86_gp_count` | GP ハンドラ呼び出し総数 |
| `v86_last_int` / `v86_last_cs` / `v86_last_ip` | 最後の INT 発行位置 |
| `v86_irq0_call_count` 〜 `v86_irq0_gp_skip_ivt` | IRQ0 注入の8種カウンタ |
| `v86_vsync_arm_count` / `v86_vsync_inject_count` | VSYNC 状態 |

---

## 12.4 リングバッファ系

| バッファ | サイズ | 場所 | 用途 |
|---------|--------|------|------|
| `v86_trace[]` (GP TRACE) | 128 件 × 12B = 1.5KB | [`v86.c:37`](../../../kernel/v86.c) | CS:IP / opcode / INT番号 / AX / CX |
| `disk_log[]` (INT 1Bh) | 64 件 × 16B = 1KB | [`v86_disk.c:70`](../../../kernel/v86_disk.c) | AH / CHS / ES:BP / Status |
| `v86_io_stats[]` | 上位16ポート | [`v86.c:155`](../../../kernel/v86.c) | ポート別アクセス回数 |

---

## 12.5 デバッグ用ヘルパ

| 関数 | 用途 |
|------|------|
| `v86_dbg_hex8/16/32` | シリアル16進出力 |
| `v86_exit_reason_str` | enum → 文字列変換 |
| `dump_v86_trace_tvram` (現在 unused) | TVRAM へ直接トレース表示 (緊急時用) |

---

## 12.6 既存の弱点 (W1〜W10)

| # | 課題 | DOS5 解析への影響 |
|---|------|----------------|
| **W1** | ファイルログ書込はセッション終了時のみ → ハードハングで全消失 | 🔴 高 |
| **W2** | GP TRACE が 128件固定 → DOS5 IPL の数千 GP のうち末尾しか残らない | 🔴 高 |
| W3 | BDA ダンプは pre-teardown 1回のみ → 時系列汚染追跡不能 | 🟡 中 |
| W4 | MEMSW (A0000:3FE2) ダンプ不在 → 設定/上書きが観測できない | 🟡 中 |
| W5 | BIOS ROM FAR CALL の専用ロガー不在 | 🟡 中 |
| W6 | 未分類 I/O ポート フォールスルー識別なし | 🟡 中 |
| W7 | IVT 差分検出なし → ゲストフック対象不明 | 🟡 中 |
| W8 | 逆アセンブル支援なし → opcode 列のみで命令意味不明 | 🟡 中 |
| W9 | ホスト側パーサ不在 → 目視解析 | 🟡 中 |
| **W10** | DOS5 期待値の参照ベースなし | 🔴 高 (A1の本質課題) |

各 W に対応する改善設計は [13_debug_tools_design.md](13_debug_tools_design.md) Tier 1〜4 を参照。

---

## 12.7 既存資産の流用可能性

新規実装を最小化するため、以下を再利用する想定:

| 既存資産 | 流用先 (13 の Tx.x) |
|---------|-------------------|
| `wb_ch` / `wb_str` / `wb_hex*` バッファヘルパ | T1.3 BDA dump / T2.1 MEMSW dump |
| `vfs_open` / `vfs_write_fd` / `vfs_seek(SEEK_END)` | T1.1 HostDrv 定期追記 |
| `v86_debug_dump_session()` のセクション追加方式 | T2.1/T2.3 新セクション追加時 |
| `v86_disk_get_log()` の取得 API パターン | T1.4 ROM CALL リング、T3.2 EOI 履歴 |
| `vfs_mkdir("/host/debug")` | 全 HostDrv 経由ログの保存先確保 |

---

## 12.8 関連ドキュメント

| ドキュメント | 関係 |
|------------|------|
| [13_debug_tools_design.md](13_debug_tools_design.md) | 機能設計 (Tier 1〜4) |
| [14_debug_tools_roadmap.md](14_debug_tools_roadmap.md) | 実装ロードマップ / メモリ消費 / KAPI |
| [11_gaps_and_verification.md](11_gaps_and_verification.md) | 上流の検証項目 |
| [02_cpu_emulation.md §2.6](02_cpu_emulation.md) | 既存 GP TRACE 仕様 |
| [03_io_port_map.md §3.6](03_io_port_map.md) | I/O 統計の現状 |
| [`kernel/v86_debug.c`](../../../kernel/v86_debug.c) | 拡張対象本体 |
| [`kernel/v86.h:88-110`](../../../kernel/v86.h) | 既存カウンタ extern 宣言 |
| [`kernel/v86_disk.h:78`](../../../kernel/v86_disk.h) | 既存 disk_log_entry |
