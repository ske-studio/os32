# 9. MS-DOS起動ロードマップ

## 9.1 目標

1. **MS-DOS 5.0A (NEC版)** がV86上でブートし、A:\>プロンプトが表示される
2. DOSコマンド (DIR, COPY, TYPE等) が動作する
3. DOSネイティブアプリケーション (VZ Editor等) が動作する
4. PC-98ネイティブゲーム (Ys, Thexder等) がブート・動作する

## 9.2 現状の問題: DOS5 IPLハング

### 症状

DOS5のFDI/実FDDからブートすると、IPLロード後にIO.SYS初期化中でハングする。
FreeDOS(98)は正常にブートするため、NEC DOS固有のBIOS/BDA依存が原因と推定。

### 調査済み事項

- IPLの先頭バイトは正常に読み込まれている (バイナリ一致確認済み)
- INT 1Bh READは正常動作 (ログ確認済み)
- IO.SYSがBDA参照でハングしている可能性が高い

### 次の調査ステップ (詳細手順)

> 詳細な未実装/要検証項目は [11_gaps_and_verification.md](11_gaps_and_verification.md) を参照。
> 以下は本節 §9.2 で言及する3つの調査ステップの具体的な実施手順。

#### Step 1. BDA 差分分析 — NP21/W (MS-DOS6.2 直接ブート) vs OS32 V86

**目的**: IO.SYS が参照する未初期化 BDA フィールドを特定する。

**前提**: NP21/W にステートセーブ機能はないため、DOS 自身に BDA を export させる。
**リファレンス環境**: MS-DOS 6.2 (NEC PC-98版) — DEBUG.COM が標準で利用可能。
DOS5 にも概ね流用できる (差異フィールドはマスクで除外)。詳細は
[13_debug_tools_design.md §13.5 T4.1](13_debug_tools_design.md#t41-np21w-リファレンス-bda-採取手順)。

**手順**:
1. **NP21/W 側リファレンス採取** (DOS6.2 直接ブート):
   ```
   NP21/W 起動 → MS-DOS 6.2 FDイメージで A:\> プロンプト到達

   A:\> DEBUG > A:\BDA.TXT
   -D 0:400 L 200
   -Q

   NP21/W 停止 → FDイメージをアンマウント

   ホスト側:
     $ mtype -i fd_dos62.fdi ::BDA.TXT > captured.txt
     $ python tools/v86_dos_bda_parse.py captured.txt > reference_dos.bin
   ```
2. **OS32 V86 側ダンプ取得** (実装は別タスク):
   - `kernel/v86_debug.c` に `v86_debug_dump_bda_named(const char *tag)` 追加
     (仕様: [13_debug_tools_design.md T1.3](13_debug_tools_design.md#t13-bda-スナップショット多段化--ホスト-diff-ツール))
   - 採取ポイント: `init` / `post_ipl` / `pre_dos` / `exit` (多段スナップショット)
   - 出力: `/host/debug/v86_bda_{tag}.bin` (HostDrv 経由)
3. **差分比較** (`tools/v86_bda_diff.py`):
   - 入力: `reference_dos.bin` + `v86_bda_{tag}.bin`
   - 出力: Markdown 差分レポート (PC-98 BDA フィールド名注釈付き)
4. 差分結果を [11.2 A1/A2](11_gaps_and_verification.md#a1-bda-差分分析-np21w-vs-v86) にフィードバック

#### Step 2. INT 18h トレース取得 — IO.SYS 初期化中の呼出パターン記録

**目的**: ハング直前に発行された BIOS 呼出シーケンスを特定する。

**機構**:
- 16B 固定長レコードを 2048-entry リングバッファに蓄積
- 80%充填 / 30 tick (300ms) / クリティカルイベント / セッション終了の各トリガで
  `/host/debug/v86_events.log` に HostDrv 追記 (シリアルは使わない)
- 既存の v86_trace[128] (テキスト) も並存させ、ヒト可読サマリは v86_diag.log に残す
- 仕様: [13_debug_tools_design.md T1.1](13_debug_tools_design.md#t11-hostdrv-定期イベント追記) /
  [T1.2](13_debug_tools_design.md#t12-gp-trace-拡張--フィルタ)

**手順**:
1. T1.1 を実装 (実装は別タスク)
2. `v86 -d DOS5.fdi` で起動 → ハング後 Ctrl+GRPH+DEL で V86 脱出
3. セッション終了時に最終 flush → `/host/debug/v86_events.log` が確定
4. ホスト側 `tools/v86_event_decode.py` で解析:
   - 末尾 200 件の kind/AH 分布 → 無限ループ検出
   - INT 1Bh 系で BX=0 (転送バイト0) 等の異常がないか
   - INT 18h AH=00h (キー入力待ち) が出ていないか ([D4](11_gaps_and_verification.md#d4-int-18h-ah00h-キー入力待ちで即v86終了-仕様の再検証))
   - ROM_CALL / TIMEOUT / UNKNOWN_OP のクリティカルイベントの有無

#### Step 3. BIOS ROM コールバック検出

**目的**: IO.SYS が BIOS ROM 内ルーチンを FAR CALL し、未対応 I/O で停止しているケースの特定。

**手順**:
1. `v86_session_end` 直前のログに **V86 終了時の CS:EIP** を必ず出力
2. CS >= 0xF000 で `V86_EXIT_BIOS_ROM` が発火している場合、IP の値で判別:
   - IP = 0x0000 → リセットベクタ到達 (正常な終了)
   - IP != 0x0000 → ROM 内ルーチンの途中 → IO.SYS が CALL したと推定
3. NP21/W で BIOS ROM (F000-FFFF) を抽出 → 該当 IP 周辺をディスアセンブル
4. 該当ルーチンが触る I/O ポートを洗い出し、未対応分を仮想化対象に追加

---

> 検証アクション項目とコード位置の対応は [11_gaps_and_verification.md §11.2 A1-A6](11_gaps_and_verification.md#112-a-高優先度--dos5-iplハング直結) を参照。

## 9.3 MS-DOS起動に必要な機能ブロック

### Phase 1: IPL → IO.SYS → MSDOS.SYS → COMMAND.COM (ブートシーケンス)

| # | 項目 | 状況 | 詳細 |
|---|------|------|------|
| 1-1 | IPLロード (Track0/Head0/Sect1) | ✅ 完了 | |
| 1-2 | INT 1Bh READ (IO.SYSロード) | ✅ 完了 | 複数セクタ対応済み |
| 1-3 | BDA FDC結果バッファ (0564h) | ✅ 完了 | IO.SYSがN値を参照 |
| 1-4 | INT 18h テキストBIOS | ✅ 完了 | AH=00h-1Bh 実装 |
| 1-5 | INT 1Bh SENSE (04h) | ✅ 完了 | 2HD/2DD判定, WP |
| 1-6 | BDA初期値 (メモリサイズ等) | ⚠️ 部分的 | DOS5固有の参照フィールド要調査 — [§9.2 Step1](#step-1-bda-差分分析--np21w-純正dos5起動状態-vs-os32-v86), [11.2 A1/A2](11_gaps_and_verification.md#a1-bda-差分分析-np21w-vs-v86) |
| 1-7 | BIOS ROM FAR CALL | ❌ 未対応 | IO.SYSがROM内ルーチンを呼ぶ場合 — [§9.2 Step3](#step-3-bios-rom-コールバック検出), [11.2 A3](11_gaps_and_verification.md#a3-bios-rom-far-call) |
| 1-8 | メモリスイッチ完全初期化 | ⚠️ 部分的 | SW1-SW4のうちSW4のみ対応 — [11.2 A5](11_gaps_and_verification.md#a5-メモリスイッチ完全初期化) |

#### Phase 1-6 補足: DOS5 が追加で参照する候補 BDA フィールド

OS32 が初期化済みのフィールドに加え、DOS5 IO.SYS が参照する可能性のあるフィールド一覧。
詳細は [11.2 A2](11_gaps_and_verification.md#a2-nec-dos5-固有-bda-フィールドの不足) 参照。

| アドレス | 名前 | 状況 | DOS5 で要求される可能性 |
|---------|------|------|----------------------|
| `0000:0455h` | BOOTPART_FLAGS | ❌ 未初期化 | 中 (パーティションフラグ) |
| `0000:0457h` | BIOS_FLAG3 | ❌ 未初期化 | 中 (HD/2MB接続フラグ) |
| `0000:0481h` | MEMSIZ | ❌ 未初期化 | 高 (メインメモリ KB単位) |
| `0000:0483h` | SHADOW_FLAG | ❌ 未初期化 | 低 |
| `0000:0598h-` | BIOS バージョン | ❌ 未初期化 | 中 |
| `0000:05A0h` | KB拡張バッファ | ❌ 未初期化 | 中 |

(候補リスト。実際の必要性は Step1 ダンプ差分で確定する)

### Phase 2: DOSプロンプト → コマンド実行

| # | 項目 | 状況 | 詳細 |
|---|------|------|------|
| 2-1 | INT 21h (DOS API) | N/A | DOSカーネル自身が処理。エミュレーション不要 |
| 2-2 | キーボード入力 (INT 09h/18h) | ✅ 完了 | kbd_bufバッファリング |
| 2-3 | 画面出力 (INT 29h) | ✅ 完了 | TVRAM直書き |
| 2-4 | FDDセクタR/W | ✅ 完了 | INT 1Bh + ポートレベルFDC |
| 2-5 | タイマ (IRQ0 → INT 08h) | ✅ 完了 | PIT分周反映済み |

### Phase 3: DOSネイティブアプリ

| # | 項目 | 状況 | 詳細 |
|---|------|------|------|
| 3-1 | EMSメモリ管理 | ❌ 未実装 | EMM386相当の動作が必要な場合 |
| 3-2 | XMSメモリ管理 | ❌ 未実装 | HIMEM.SYS相当 |
| 3-3 | プリンタ (INT 1Ah) | ❌ 未実装 | ack応答で十分な可能性 |
| 3-4 | マウス (INT 33h) | ❌ 未実装 | バスマウスI/O |

### Phase 4: PC-98ネイティブゲーム

| # | 項目 | 状況 | 詳細 |
|---|------|------|------|
| 4-1 | FM音源パススルー | ✅ 完了 | OPN直接アクセス |
| 4-2 | GVRAM直接描画 | ✅ 完了 | ページテーブルでマップ |
| 4-3 | GRCG/EGCパススルー | ✅ 完了 | IOビットマップ許可 |
| 4-4 | VSYNCポーリング | ✅ 完了 | GPカウントベースbit5トグル |
| 4-5 | VSYNC割り込み (INT 0Ah) | ✅ 完了 | 50Hz注入 |
| 4-6 | D88コピープロテクション | ✅ 完了 | fdc_treg/fdc_hd分離 |
| 4-7 | ジョイスティック | ⚠️ 部分的 | SSGポートA/B パススルー |
| 4-8 | BEEP音源BGM | ✅ 完了 | port 0x37 パススルー |

## 9.4 パススルー設計の原則

### 原則1: OS32共有デバイスは完全仮想化

```
OS32が使用中:
  PIC → OS32のIRQルーティングが壊れる
  PIT Counter#0 → OS32の100Hzタイマが壊れる
  キーボード → スキャンコード読み取りが競合
  RS-232C → rshellシリアル通信が壊れる
  HostDrv → ファイルI/Oが壊れる
```

### 原則2: ゲスト専用デバイスは直接パススルー

```
FM音源:
  リアルタイム性が最重要。GPオーバーヘッド (数µs/命令) は
  FM音源のレジスタ書き込みタイミングを破壊する。
  特にSSGの周波数レジスタは連続書き込みが必要。

GVRAM:
  メモリマップドI/O。ページテーブルで直接マッピング。
  GPフォルトではなくページフォルトで制御可能だが、
  パフォーマンス上パススルーが最適。

GRCG/EGC:
  グラフィックアクセラレータ。描画処理のパフォーマンスに直結。
```

### 原則3: 状態監視が必要なデバイスは条件付き仮想化

```
GDCステータス (0x60/0xA0):
  VSYNCビット(bit5)はゲームのタイミング同期に必須。
  実ハードウェアのVSYNC信号はGPオーバーヘッドで見逃すため、
  GPカウントベースの仮想トグルが必要。

FDC:
  ディスクイメージの場合 → 完全仮想化 (実FDCに触れない)
  実FDDの場合 → 実ハードウェアにパススルー
```

## 9.5 既知の制約・限界

### GPオーバーヘッド

V86モードの全I/O命令は #GP を発生させる。GPハンドラの処理時間は
1命令あたり数µs〜数十µs。FM音源以外のI/Oは許容範囲だが、
大量のI/Oを行うデバイスドライバでは性能低下が顕著になる可能性がある。

IOビットマップでパススルーを許可すれば GPフォルトは発生しないが、
仮想化が必要なデバイスには適用できない。

### IRQ0注入の上限

OS32のベースタイマは100Hz。ゲストがPIT Counter#0を高速に設定しても
100Hz以上のIRQ0注入は不可能。ただし、MS-DOSや大半のゲームは
100Hz (デフォルト) または 50-60Hz で動作するため、実用上の問題は少ない。

### BIOS ROM依存

V86モードでは物理BIOS ROM (0xF0000-0xFFFFF) をR/Oでマッピングしている。
ゲストがBIOS ROMのルーチンをFAR CALLする場合、そのコード内のI/O命令も
#GPでトラップされ、エミュレーション対象となる。

ただし、BIOS ROM内部のI/Oアクセスパターンが未知の場合、予期しない動作や
ハングの原因になる。BIOS ROMのディスアセンブルと照合が必要。
