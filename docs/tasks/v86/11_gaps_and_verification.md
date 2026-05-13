# 11. 未実装・要検証項目 一覧 (MS-DOSブート観点)

> 本ドキュメントは、`docs/tasks/v86/` 全体を横断して **未実装** または **要検証** な
> 項目を洗い出したものである。特に MS-DOS 5 (NEC版) のV86ブートが現在ハングして
> いる原因仮説と、次に取るべき検証アクションを項目ごとに整理する。
>
> **PC-98 と PC/AT はアーキテクチャが根本的に異なる**点に注意。
> I/Oアドレス、BIOS仕様、BDAレイアウト、INT vector マップが全く別である。
> MS-DOS の IO.SYS も NEC版は PC/AT版とは別実装で、PC-98 固有 BIOS と
> BDA 領域に強く依存する。

## 11.1 サマリ表

| カテゴリ | 件数 | 概要 |
|---------|-----|------|
| A. 高優先度 | 6 | DOS5 IPLハング直結 |
| B. 中優先度 | 6 | DOS起動後で必要 |
| C. 低優先度 | 6 | DOSネイティブアプリで必要 |
| D. 設計整合性 | 6 | コード/ドキュメントの確認・要検証 |
| E. PC-98/PC/AT 差分 | 7 | 暗黙のPC/AT前提混入チェックリスト |
| **計** | **31** | |

凡例:
- ステータス: ❌ 未実装 / ⚠️ 部分的・要検証 / ✅ 実装済み (確認のみ)
- DOS5 影響度: 🔴 高 / 🟡 中 / 🟢 低

---

## 11.2 A. 高優先度 — DOS5 IPLハング直結

### A1. BDA 差分分析 (NP21/W vs V86)

- **ステータス**: ⚠️ 計画のみ・未実行
- **該当コード**: [`kernel/v86_mem.c:57-205`](../../../kernel/v86_mem.c) (`bda_defaults[]` + 個別代入)
- **該当ドキュメント**: [§9.2](09_msdos_roadmap.md) / [§4.4](04_memory.md)
- **DOS5 影響度**: 🔴 高 (IPL ハング直結)
- **根拠**: DOS5 IO.SYS 起動中ハング、FreeDOS は同 BDA 初期化で起動成功 →
  IO.SYS が参照する未初期化フィールドが存在する可能性が極めて高い。
- **次の検証アクション**:
  1. **リファレンス採取**: NP21/W で **MS-DOS 6.2 (NEC PC-98版) を直接ブート** →
     `DEBUG > A:\BDA.TXT` でメモリ 0x0400-0x07FF を採取 → mtools で抽出
     ([13_debug_tools_design.md §13.5 T4.1 手順案1'](13_debug_tools_design.md#手順案-1--debugcom--ファイルリダイレクト-第一推奨))
  2. **OS32 V86 採取**: v86_mem_setup() 直後・IPL読込直後・終了直前の3点でバッキングRAMダンプ
     ([T1.3 BDA スナップショット多段化](13_debug_tools_design.md#t13-bda-スナップショット多段化--ホスト-diff-ツール))
  3. **差分比較**: `tools/v86_bda_diff.py` で bytewise diff、PC-98 BDAフィールド注釈付き
  4. セグメント 0x0060 (BIOS作業領域) も同様に比較
  - **DOS6.2 vs DOS5 差異**: BIOSバージョン領域 (0x0598h-) や RTC作業領域はマスクで除外
- **実装は別タスク** (本ドキュメントでは仕様化のみ)

### A2. NEC DOS5 固有 BDA フィールドの不足

- **ステータス**: ⚠️ 未調査
- **該当コード**: [`kernel/v86_mem.c:181`](../../../kernel/v86_mem.c) (BOOT_DEV=0x90のみ)
- **該当ドキュメント**: [§9.3 Phase 1-6](09_msdos_roadmap.md) 「DOS5固有の参照フィールド要調査」
- **DOS5 影響度**: 🔴 高
- **根拠**: 現在初期化されている主要BDAは FreeDOS 動作実績ベースであり、
  DOS5 IO.SYS が追加で参照するフィールドが存在する可能性が高い。
- **候補フィールド** (PC9800Bible / NP21/W bios09.c より):
  - `0000:0455h` BOOTPART_FLAGS — ブートパーティションフラグ
  - `0000:0457h` BIOS_FLAG3 — bit2(HD ATTACH), bit1(2 MB ATTACH)
  - `0000:0458h` BIOS_FLAG5 — 既に追加済み (値の正当性要検証)
  - `0000:0481h` MEMSIZ — メインメモリサイズ (KB単位)
  - `0000:0482h` SCSI_HD — 既に追加済み (値の正当性要検証)
  - `0000:0483h` SHADOW_FLAG — シャドウRAM
  - `0000:055Dh` SASI/IDE — 既に追加済み (値の正当性要検証)
  - `0000:0598h-` BIOS バージョン情報
  - `0000:05A0h` キーボードバッファ拡張
- **次の検証アクション**:
  1. A1 のダンプ差分から、欠落フィールドを特定
  2. `kernel/v86_bda.h` の定数定義と突き合わせ、定義漏れも洗う
  3. PC9800Bible §4-5 / UNDOC memsys.md と照合

### A3. BIOS ROM FAR CALL

- **ステータス**: ❌ 検証要・未対応の可能性
- **該当コード**: [`kernel/v86.c`](../../../kernel/v86.c) (CS>=0xF000 で `v86_exit_request`、BIOS_ROM 終了扱い)
- **該当ドキュメント**: [§9.3 Phase 1-7](09_msdos_roadmap.md) / [§8.5](08_session_lifecycle.md) `V86_EXIT_BIOS_ROM`
- **DOS5 影響度**: 🔴 高
- **根拠**: 現実装は CS が 0xF000 以上に達した時点で V86 を「リセットベクタ到達」
  として強制終了する。しかし MS-DOS IO.SYS は BIOS ROM 内ルーチンを正常な手順で
  FAR CALL することがあり、これも一律で終了扱いになっている可能性。
- **次の検証アクション**:
  1. V86 終了直前の CS:IP を必ずログ出力 (FFFF:0000 か他のアドレスか確認)
  2. CS >= 0xF000 でも、IP が ROM 内有効ルーチン位置の場合は実行継続するロジックを検討
  3. BIOS ROM ディスアセンブルし、IO.SYS が呼び得るルーチンを特定

### A4. INT 18h 呼び出しトレース取得

- **ステータス**: ⚠️ 機構は実装済み・実行未
- **該当コード**: [`kernel/v86.c:142-146`](../../../kernel/v86.c) (v86_int_count等)、`v86_trace[128]`
- **該当ドキュメント**: [§9.2](09_msdos_roadmap.md) / [§2.6](02_cpu_emulation.md)
- **DOS5 影響度**: 🔴 高
- **根拠**: 既に 128件のトレースリングバッファが存在するが、DOS5 ハング直前の
  実ログが採取されていない。実機ログがないと仮説検証ができない。
- **次の検証アクション**:
  1. DOS5 ブートを実行し、ハング後 Ctrl+GRPH+DEL で脱出 → トレースダンプ取得
  2. AH 値の分布、最後の数件、ループ検出を分析
  3. リングバッファサイズが 128 で足りない場合は拡張 (別タスク)

### A5. メモリスイッチ完全初期化

- **ステータス**: ⚠️ 部分的
- **該当コード**: [`kernel/v86_mem.c:222-231`](../../../kernel/v86_mem.c)
- **該当ドキュメント**: [§9.3 Phase 1-8](09_msdos_roadmap.md) / [§4.5](04_memory.md)
- **DOS5 影響度**: 🟡 中〜高
- **根拠**: 現在 MEMSW1=0x48 / MEMSW2=0x01 / MEMSW3=0x04、MEMSW4-6=0 で固定。
  PC9800Bible / UNDOC memsys.md に従い、DOS5 IO.SYS が参照する設定 (CRT周波数、
  クロック系、メモリ構成) が完全か未確認。
- **次の検証アクション**:
  1. NP21/W 標準設定における 0xA000:3FE2-3FF7 (MEMSW全域) ダンプ取得
  2. 現在のOS32初期値との差分を確認
  3. SW4-6 に意味のある値が必要か (機種固有設定) を判定

### A6. INT 1Bh AH=04h SENSE 戻り値の正当性

- **ステータス**: ⚠️ 値仕様の検証要
- **該当コード**: [`kernel/v86_disk.c:431-464`](../../../kernel/v86_disk.c)
- **該当ドキュメント**: [§6.3 機能04h](06_bios_emulation.md) / [§7.3 SENSE](07_fdc_virtualization.md)
- **DOS5 影響度**: 🔴 高 (FreeDOS 動作実績はあるが、DOS5 解釈が異なる可能性)
- **根拠**: コメントに「IO.SYSはこの値でディスクフォーマット(CH)を決定する」と
  明記されている。FreeDOS は 0x01 ベース、DOS5 は 0x09 (bit0|bit3) を期待する
  可能性。
- **次の検証アクション**:
  1. NP21/W 上で純正 DOS5 ブート → INT 1Bh AH=04h の戻り値ログを取得
  2. 現在の OS32 戻り値と差分を確認
  3. PC9800Bible §2-9 表2-34 と照合

---

## 11.3 B. 中優先度 — DOSが起動した後で必要

### B1. HDD ブート (INT 1Bh DA=A0h SASI / 0x80 IDE)

- **ステータス**: ❌ 未対応 (FDD のみ)
- **該当コード**: [`kernel/v86_disk.c`](../../../kernel/v86_disk.c) / [`drivers/loop_dev.c`](../../../drivers/loop_dev.c)
- **該当ドキュメント**: [§9.2 #3](../../V86_STATUS.md)
- **DOS5 影響度**: 🟡 中 (FDDブート優先のため後回し)
- **次の検証アクション**: 別タスク `chs_native` 系で進行中の可能性あり。要確認

### B2. INT 21h DOS API は本当に不要か

- **ステータス**: ⚠️ 情報扱い・実機ログでの裏付けなし
- **該当コード**: 該当なし (DOSカーネル内で処理される前提)
- **該当ドキュメント**: [§9.2 #1](../../V86_STATUS.md) / [§6.1](06_bios_emulation.md)
- **DOS5 影響度**: 🟡 中
- **根拠**: 「DOSカーネル自身が処理。エミュレーション不要」と仮定しているが、
  IO.SYS 初期化フェーズでは INT 21h ハンドラがまだ未設定 → ダミーIVT直行 →
  CF=1 + AH=0x86 で返している。これが想定外動作の原因になり得る。
- **次の検証アクション**:
  1. v86_trace ログで INT 21h 呼び出しが IO.SYS 初期化中に発生していないか確認
  2. 発生していた場合、ダミーIVT応答ではなく "ack" 応答 (AH=0 / CF=0) も試す

### B3. INT 1Ah (PC/AT タイマBIOS)

- **ステータス**: ❌ 未実装 (PC-98 では INT 08h/1Ch を使用する)
- **該当ドキュメント**: [§6.8](06_bios_emulation.md)
- **DOS5 影響度**: 🟢 低 (NEC版 DOS5 は使わない可能性大)
- **次の検証アクション**: v86_trace で INT 1Ah 発行有無を確認。発行ありなら ack 応答実装

### B4. INT 1Eh プリンタBIOS

- **ステータス**: ❌ 未実装
- **該当ドキュメント**: [§6.8](06_bios_emulation.md)
- **DOS5 影響度**: 🟢 低 (CONFIG.SYS デバイスロード時に呼ばれる可能性)
- **次の検証アクション**: v86_trace で確認 → 必要なら ack 応答実装

### B5. INT 10h CRTモードBIOS

- **ステータス**: ❌ 未実装 (「一部ゲームが使用、要調査」)
- **該当ドキュメント**: [§6.8](06_bios_emulation.md)
- **DOS5 影響度**: 🟢 低
- **次の検証アクション**: v86_trace で確認

### B6. INT 18h AH=0Ah カラー/モノクロ切替

- **ステータス**: ⚠️ ack のみ (切替未実装)
- **該当コード**: [`kernel/v86_bios.c:207`](../../../kernel/v86_bios.c)
- **該当ドキュメント**: [§6.2 AH=0Ah](06_bios_emulation.md)
- **DOS5 影響度**: 🟢 低 (DOS は通常 80x25 固定で OK)
- **次の検証アクション**: VZ Editor 等で必要になった時点で実装

---

## 11.4 C. 低優先度 — DOSネイティブアプリで必要

### C1. EMS メモリ管理 (EMM386 相当)

- **ステータス**: ❌ 未実装
- **該当ドキュメント**: [§9.3 Phase 3-1](09_msdos_roadmap.md)
- **DOS5 影響度**: 🟢 低 (本体ブートには不要)

### C2. XMS メモリ管理 (HIMEM.SYS 相当)

- **ステータス**: ❌ 未実装
- **該当ドキュメント**: [§9.3 Phase 3-2](09_msdos_roadmap.md)
- **DOS5 影響度**: 🟢 低

### C3. INT 33h マウス

- **ステータス**: ❌ 未実装
- **該当ドキュメント**: [§9.3 Phase 3-4](09_msdos_roadmap.md)
- **DOS5 影響度**: 🟢 低

### C4. DOSエクステンダ用 0x0F 2バイトオペコード

- **ステータス**: ❌ 未対応 (LGDT/LIDT/LMSW/MOV CRn)
- **該当コード**: [`kernel/v86.c:891`](../../../kernel/v86.c) 未対応で V86 終了
- **該当ドキュメント**: [§2.7](02_cpu_emulation.md) / [§2.3](../../V86_STATUS.md)
- **DOS5 影響度**: 🟢 低 (MS-DOS本体には不要、後回し方針)

### C5. INSD/OUTSD (32bit文字列I/O)

- **ステータス**: ❌ 未実装
- **該当ドキュメント**: [§2.7](02_cpu_emulation.md)
- **DOS5 影響度**: 🟢 低

### C6. LOCKプレフィックス付きメモリ操作

- **ステータス**: ⚠️ LOCK 読み飛ばしのみ
- **該当ドキュメント**: [§2.3](../../V86_STATUS.md)
- **DOS5 影響度**: 🟢 低

---

## 11.5 D. 設計整合性・要検証

### D1. 未分類I/Oポートが実HWに直接フォールスルー

- **ステータス**: ⚠️ TODO 明記済み・分析未実行
- **該当コード**: [`kernel/v86.c`](../../../kernel/v86.c) `v86_in8_checked` / `v86_out8_checked`
- **該当ドキュメント**: [§3.6](03_io_port_map.md) (「TODO: 統計分析必要」)
- **DOS5 影響度**: 🟡 中 (DOS が触る未知ポートで OS32 を破壊する可能性)
- **想定される影響範囲**:
  - 0x5C/0x5E (カレンダ詳細)
  - 0x80 (GDC バス制御)
  - 0xBE (1MB/640KB FDC切替) — D5参照
  - 0xCD/0xCF (システム制御)
- **次の検証アクション**:
  1. v86_io_stat_record を見直し、ポート単位の R/W 回数を出力
  2. 未分類ポートで「実HW直送」となったポートをリスト化
  3. それぞれを「パススルー安全」「保護必要」「仮想化必要」に分類

### D2. VSYNC フリーラン注入が未実装

- **ステータス**: ❌ コード内 TODO
- **該当コード**: [`kernel/v86_vsync.c:162`](../../../kernel/v86_vsync.c) TODO
- **該当ドキュメント**: [§5.4](05_irq_injection.md)
- **DOS5 影響度**: 🟢 低 (DOS5 が INT 0Ah をフックする頻度が低いため)
- **次の検証アクション**: v86_trace で IVT[0x0A] フック有無を確認

### D3. ダミーIVT セグメント値のドキュメント不整合

- **ステータス**: ⚠️ ドキュメント-実装の検証要
- **該当コード**: [`kernel/v86.c:649-653`](../../../kernel/v86.c) (`V86_IS_DUMMY_IVT` マクロ)
- **該当ドキュメント**:
  - [§4.3](04_memory.md) — `0x003F:0x0000` と記述
  - [`kernel/v86_mem.c`](../../../kernel/v86_mem.c) — `IVT_HANDLER_BASE = 0x03F0` (= `0x003F:0x0000`)
  - [`kernel/v86.c:646` コメント](../../../kernel/v86.c) — `0x0050:0x0000` と記述
- **DOS5 影響度**: 🟢 低 (実装は v86_mem.c と一致しているとみられる)
- **次の検証アクション**:
  1. `V86_IS_DUMMY_IVT` マクロの定義箇所を特定
  2. v86.c のコメントを実装と一致するよう修正
  3. 実害がないか v86_trace で確認

### D4. INT 18h AH=00h "キー入力待ちで即V86終了" 仕様の再検証

- **ステータス**: ⚠️ 修正履歴あり・現状の挙動要確認
- **該当コード**: [`kernel/v86.c:588-593`](../../../kernel/v86.c) (rc=-2 で V86 継続のはず)
- **該当ドキュメント**: 旧 `_archive/freedos98_boot_debug_report.md §1`
- **DOS5 影響度**: 🟡 中 (IPLハング時のエラー文字が読めなくなる)
- **次の検証アクション**:
  1. v86_bios.c の AH=00h ハンドラが rc=-2 を返すよう実装されているか確認
  2. v86.c が rc=-2 を「EIP加算なしでINTを再実行」として扱うか確認
  3. DOS5 IPL の `boot_error` シミュレーション (HELLO.COM相当でAH=00h呼出) で検証

### D5. 0xBE FDC モード切替 (1MB/640KB)

- **ステータス**: ❌ 実FDD経路では未対応
- **該当コード**: [`kernel/v86_fdc.c`](../../../kernel/v86_fdc.c) / [`drivers/fdc.c`](../../../drivers/fdc.c)
- **該当ドキュメント**: [§7](07_fdc_virtualization.md) (言及なし) / 旧 `V86_DISK_ISSUE.MD`
- **DOS5 影響度**: 🟡 中 (実FDD DOS5 2DD ブートで影響)
- **次の検証アクション**:
  1. 07_fdc_virtualization.md に 0xBE 切替の現状 (未対応) を明記
  2. 実FDD 2DD ブート時のフロー検証

### D6. 「FreeDOS OK・DOS5 NG」差分の本質説明欠如

- **ステータス**: ⚠️ 仮説のみ・確証なし
- **該当ドキュメント**: [§9.2](09_msdos_roadmap.md) 「NEC DOS固有の BIOS/BDA依存が原因と推定」
- **DOS5 影響度**: 🔴 高 (デバッグ方針の根幹)
- **次の検証アクション**:
  1. A1/A4 のダンプ・トレース結果から、FreeDOS の `_int29_main` / `init_crt` /
     `dsk_init` フローと DOS5 IO.SYS の挙動を対比
  2. 差分を「FreeDOS は触れない / DOS5 は触れる」フィールドとして列挙
  3. 旧 `freedos98_init_flow.md` の §4 (ハング仮説) と突き合わせ

---

## 11.6 E. PC-98 / PC/AT 差分 (確認チェックリスト)

各項目について、ドキュメント記述が PC-98 仕様に正しく従っているかチェック。
PC/AT の暗黙前提が混入していないか定期的に確認すること。

| # | 項目 | PC-98 | PC/AT | 該当ドキュメント | 状況 |
|---|------|-------|-------|----------------|------|
| E1 | PIC ポート | マスタ=00h/02h, スレーブ=08h/0Ah | マスタ=20h/21h, スレーブ=A0h/A1h | [§3.2 PIC](03_io_port_map.md) | ✅ 正 |
| E2 | PIT ポート | 71h/73h/75h/77h | 40h-43h | [§3.2 PIT](03_io_port_map.md) | ✅ 正 |
| E3 | FDC ポート | 1MB=90h/92h/94h, 切替=BEh, 640KB=C8h/CAh/CCh | 3F2-3F7 | [§3.2 FDC](03_io_port_map.md) | ⚠️ 0xBE 言及なし (D5) |
| E4 | BDA レイアウト | 0x0400-0x07FF (PC-98独自) | 0x0400-0x04FF | [§4.4](04_memory.md) | ✅ 正 |
| E5 | INT vector | INT 08h=TMR, 09h=KBD, 0Ah=VSYNC, 0Bh+=拡張 | INT 08h=TMR, 09h=KBD, 0Ah=スレーブPIC | [§5](05_irq_injection.md) | ⚠️ INT 0Bh-17h (NDP/HDD/FDD/FM/マウス) が未言及 |
| E6 | キーボードI/F | 8251 @41h/43h, IRQ1, 19200bps | 8042 @60h/64h, IRQ1 | [§3.2 KBD](03_io_port_map.md) | ✅ 正 |
| E7 | MS-DOS版 | NEC PC-98 専用版 (NECDOS) | PC/AT 互換版 (MS-DOS) | [§9.1 #1](09_msdos_roadmap.md) | ✅ 「MS-DOS 5.0A (NEC版)」と明記 |

---

## 11.7 次の検証アクション (実装は別タスク)

本ドキュメントは洗い出しに留め、以下の検証機構は **別タスクで実装** すること。
ここでは仕様 (取得するデータの形式) のみを定義する。

### 11.7.1 BDA dump 取得手順 (仕様のみ)

**機能**: V86 セッション中、複数の節目で BDA 全域 (0x00400-0x007FF) を
HostDrv 経由でファイルに書き出す。詳細仕様は
[13_debug_tools_design.md T1.3](13_debug_tools_design.md#t13-bda-スナップショット多段化--ホスト-diff-ツール)。

**実装イメージ**:
- `kernel/v86_debug.c` に `v86_debug_dump_bda_named(const char *tag)` 関数を追加
- バッキングRAM の 0x00400-0x005FF (512B) を raw バイナリで `/host/debug/v86_bda_{tag}.bin` に出力
- 採取タグ: `init` / `post_ipl` / `pre_dos` / `exit`
- V86 終了時にも自動呼出

**比較ツール**: `tools/v86_bda_diff.py` で NP21/W リファレンス (DOS6.2) と
bytewise diff、PC-98 BDA フィールド名注釈付き markdown 出力。

### 11.7.2 INT 18h トレース取得手順 (仕様のみ)

**機能**: V86 セッション中の GP / INT / IRQ / ROM_CALL / EXIT 等の全イベントを
HostDrv 経由でバイナリログに連続記録する。詳細仕様は
[13_debug_tools_design.md T1.1](13_debug_tools_design.md#t11-hostdrv-定期イベント追記) +
[T1.2](13_debug_tools_design.md#t12-gp-trace-拡張--フィルタ)。

**実装イメージ**:
- 16B 固定長レコードの 2048-entry リングバッファ → `/host/debug/v86_events.log`
- Flush: 80%充填 / 300ms (30 tick) 毎 / クリティカルイベント時 / セッション終了時
- 既存の v86_trace[128] (テキスト) も並存させ、ヒト可読サマリは v86_diag.log に出力

**分析**: ホスト側 `tools/v86_event_decode.py` で kind 別フィルタ / tick 範囲 /
AH ヒストグラム集計 → ハング直前の挙動を特定。

### 11.7.3 NP21/W との BDA / トレース比較手順 (仕様のみ)

**手順**:
1. NP21/W で MS-DOS 6.2 (NEC版) を **直接ブート** → `DEBUG > A:\BDA.TXT` でBDA採取
   ([13_debug_tools_design.md §13.5 T4.1 手順案1'](13_debug_tools_design.md#手順案-1--debugcom--ファイルリダイレクト-第一推奨))
2. ホストで mtools 抽出 → `reference_dos.bin`
3. OS32 で `v86 -d DOS5.fdi` → ハング → `/host/debug/v86_bda_*.bin` と `v86_events.log` を採取
4. `tools/v86_bda_diff.py` で差分レポート生成
5. 差分から A1/A2/A4/A5 の各項目に対応

---

## 11.8 参照ドキュメント

| ドキュメント | 内容 |
|-------------|------|
| [01_architecture.md](01_architecture.md) | 全体構成 |
| [02_cpu_emulation.md](02_cpu_emulation.md) | #GP ハンドラ |
| [03_io_port_map.md](03_io_port_map.md) | I/Oポートマップ |
| [04_memory.md](04_memory.md) | メモリ管理 / BDA |
| [05_irq_injection.md](05_irq_injection.md) | IRQ仮想化 |
| [06_bios_emulation.md](06_bios_emulation.md) | BIOSエミュレーション |
| [07_fdc_virtualization.md](07_fdc_virtualization.md) | FDC仮想化 |
| [08_session_lifecycle.md](08_session_lifecycle.md) | セッション管理 |
| [09_msdos_roadmap.md](09_msdos_roadmap.md) | MS-DOSロードマップ (本ドキュメントの母体) |
| [10_test_matrix.md](10_test_matrix.md) | テストマトリクス |
| [../../V86_STATUS.md](../../V86_STATUS.md) | 上位サマリ |
| [../../PC9800Bible/4-7_MS-DOS.md](../../../../docs/PC9800Bible/4-7_MS-DOS.md) | DOSファンクション一覧 |
| [../../PC9800Bible/2-9_ディスク.md](../../../../docs/PC9800Bible/2-9_ディスク.md) | INT 1Bh仕様 |
| `../../../../docs/undocumented/memsys.md` | BDA全定義 |
