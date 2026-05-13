# 13. デバッグツール 機能設計

> 本ドキュメントは [12_debug_tools_inventory.md](12_debug_tools_inventory.md) で
> 洗い出した弱点 (W1〜W10) と [11_gaps_and_verification.md](11_gaps_and_verification.md)
> の検証項目 (A1〜A6, B1〜B6, C1〜C6, D1〜D6, E1〜E7) に対応するための **機能設計**。
> 実装の順序・工数・メモリ消費は [14_debug_tools_roadmap.md](14_debug_tools_roadmap.md) を参照。

## 13.1 サマリ

| Tier | 件数 | 概要 | フェーズ |
|------|-----|------|---------|
| Tier 1 | 4 | DOS5 ハング解析に直結 | Phase 1 |
| Tier 2 | 5 | 検証カバレッジ拡張 | Phase 2 |
| Tier 3 | 6 | 深部解析・将来用 (T3.6 = フォールバック) | Phase 3 |
| Tier 4 | 3 | ホスト側ツール | Phase 1+4 |
| **計** | **18** | | |

### §11 検証項目との対応マトリクス

| §11 項目 | Tier 1 | Tier 2 | Tier 3 |
|---------|--------|--------|--------|
| A1 BDA 差分 | T1.3 | T2.5 | |
| A2 NEC DOS固有BDA | T1.3 | T2.4/T2.5 | |
| A3 BIOS ROM FAR CALL | T1.4 | | |
| A4 INT 18h トレース | T1.1/T1.2 | | T3.1/T3.4/T3.5 |
| A5 メモリスイッチ | | T2.1 | |
| A6 INT 1Bh SENSE | (既存 disk_log) | T2.5 | |
| D1 未分類 I/O | | T2.2 | T3.3 |
| D2 VSYNC | | T2.3 | |
| D3 ダミーIVT 不整合 | | T2.3 | |
| D6 FreeDOS/DOS5 差分 | T1.3 | T2.3/T2.4 | |

### 各 Tx.x の記述構造

各機能は以下の統一テンプレートで記述する:

- **対応 §11 項目** — 紐づく検証項目
- **目的** — 解決したい問題
- **仕様** — 機能の詳細
- **実装ヒント** — 既存コードの再利用箇所・コード差分の規模感
- **依存** — 他 Tx.x への前提・後段
- **検証方法** — 実装後の動作確認手順
- **留意点** — 落とし穴・制約

---

## 13.2 Tier 1 — DOS5 ハング解析に直結 (4件)

### T1.1 HostDrv 定期イベント追記

**対応 §11 項目**: A3 / A4 / W1 (ハードハング)
**目的**: V86 セッション中の GP/INT/IRQ イベントを最小オーバーヘッドで時系列保存し、ハング直前のコンテキストまで残す。

**仕様**: シリアル送出ではなく **`/host/debug/v86_events.log` への HostDrv 定期追記** を採用する (HostDrv は OS32 上で最速の I/O 経路)。

#### イベントレコード形式

固定長 16 バイト (バイナリ) — テキスト整形はホスト側パーサで実施。
バイナリ採用理由: 1行 ~20byte の wb_str 整形より高速 + サイズ削減。

```c
enum v86_event_kind {
    V86_EV_GP        = 0,
    V86_EV_INT       = 1,
    V86_EV_IRQ_INJ   = 2,
    V86_EV_ROM_CALL  = 3,  /* critical (即時flush) */
    V86_EV_EXIT      = 4,  /* critical */
    V86_EV_TIMEOUT   = 5,  /* critical */
    V86_EV_UNKNOWN_OP= 6,  /* critical */
};

struct v86_event {
    u32 tick;        /* tick_count スナップ */
    u16 cs;          /* フォルト時の CS */
    u16 ip;          /* フォルト時の IP */
    u8  kind;        /* enum v86_event_kind */
    u8  arg1;        /* INT番号 / IRQ番号 / EXIT理由 */
    u8  arg2;        /* AH / VECTOR */
    u8  arg3;        /* AL */
    u16 cx;          /* ECX下位16bit */
    u16 reserved;
};  /* 16 bytes */
```

#### バッファ構造

```c
#define V86_EVENT_BUF_SIZE 2048             /* = 32KB */
static struct v86_event ev_buf[V86_EVENT_BUF_SIZE];
static volatile u32 ev_head = 0;            /* 次に書く位置 */
static volatile u32 ev_lost = 0;            /* オーバーフローで失われた件数 */
```

#### Flush トリガ (ハイブリッド)

| トリガ | 閾値 | 目的 |
|--------|------|------|
| **バッファ80%充填** | head ≥ 1638 (= 2048 × 0.8) | 取りこぼし回避 |
| **定期タイマ** | 30 tick = 300ms 経過 | スループット重視・オーバーヘッド最小化 |
| **クリティカルイベント** (即時) | kind = ROM_CALL / EXIT / TIMEOUT / UNKNOWN_OP | ハングしてもイベントが残る |
| **セッション終了時** | run_core 末尾 | 既存 v86_diag.log と同じ最終 flush |

#### Flush 実装スケッチ

```c
static int ev_fd = -1;
static u32 last_flush_tick = 0;

static void v86_event_flush(void) {
    u32 cli_flags;
    u32 to_write;

    if (ev_fd < 0 || ev_head == 0) return;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(cli_flags));
    to_write = ev_head * sizeof(struct v86_event);
    __asm__ volatile("pushl %0; popfl" :: "r"(cli_flags));

    vfs_write_fd(ev_fd, ev_buf, to_write);
    ev_head = 0;
    last_flush_tick = tick_count;
}
```

#### ファイル管理

| 項目 | 仕様 |
|------|------|
| パス | `/host/debug/v86_events.log` |
| open フラグ | `O_WRONLY \| O_CREAT \| O_TRUNC` (セッション毎にゼロから) |
| 開閉 | `v86_debug_write_header` で open / セッション終了で close |
| 既存 `v86_diag.log` との関係 | 別ファイル。テキスト系サマリは diag、生イベントは events |

**実装ヒント**:
- 既存 [`kernel/v86_debug.c:96-117`](../../../kernel/v86_debug.c) の `log_open` / `wb_flush` パターンをそのまま流用
- [`fs/fd_redirect.c:60-65`](../../../fs/fd_redirect.c) の `vfs_seek(fd, 0, SEEK_END)` 追記実装例
- 新規追加コード規模: ~200行 (kernel/v86_event.c 新設)

**依存**:
- 前提: なし (既存 VFS で完結)
- 後段: T1.2 (GP TRACE) / T1.4 (ROM CALL) はこの event_record() を呼び出す形に統一可

**検証方法**:
1. `v86 -d` で v86_events.log が生成されるか確認
2. `ls -la /host/debug/v86_events.log` でサイズが ~32KB 以下かつイベント数分の倍数 (16B単位) であることを確認
3. ハードハング時 (例: タイムアウト発火) でも最後の 80% の event が残っているか
4. `tools/v86_event_decode.py < v86_events.log` でテキスト整形が通る

**留意点**:
- HostDrv ポート (0x7EC/0x7EE) は V86 ゲストから見ると保護ポート扱いだが、本機能の flush は **#GP/IRQ ハンドラ復帰前の Ring 0 文脈** で実行されるため、ゲストの保護とは無関係に書き込める

---

### T1.2 GP TRACE 拡張 + フィルタ

**対応 §11 項目**: A4 / W2
**目的**: 128件制限を解消し、INT 18h など特定 INT のサブ集合だけを追跡可能にする。

**仕様**:

#### バッファ拡張

```c
/* kernel/v86.c */
#define V86_TRACE_SIZE 1024                /* 旧 128 → 1024 */

struct v86_trace_entry {
    u32 tick;          /* [+] 新規 — 時系列復元用 */
    u16 cs;
    u16 ip;
    u8  opcode;
    u8  intno;
    u8  ah;
    u8  al;
    u16 cx;
    u16 reserved;      /* [+] 16B align */
};  /* 16B (旧 12B) */
```

メモリ消費: 12B × 128 → 16B × 1024 = **16KB増加** (1.5KB → 16.5KB)

#### フィルタ API

```c
/* グローバル状態 */
extern int v86_trace_filter_int;     /* -1=全件, 0-255=特定INT */
extern u16 v86_trace_filter_cs_min;  /* 0=無効 */
extern u16 v86_trace_filter_cs_max;  /* 0=無効 */

/* 制御API (KAPI 経由でユーザーランドから呼べる) */
void v86_trace_set_int_filter(int int_no);    /* int_no=-1 で解除 */
void v86_trace_set_cs_range(u16 lo, u16 hi);  /* lo=hi=0 で解除 */
```

GP ハンドラ末尾の trace 記録ロジック:

```c
static void v86_trace_record(...) {
    /* フィルタチェック */
    if (v86_trace_filter_int >= 0 && intno != v86_trace_filter_int) return;
    if (v86_trace_filter_cs_min && (cs < v86_trace_filter_cs_min ||
                                     cs > v86_trace_filter_cs_max)) return;
    /* 既存と同じ記録ロジック */
    ...
}
```

#### AH ヒストグラム

INT 番号 × AH 値 別の頻度を別途集計:

```c
/* 案A: フル u16 [256][256] = 128KB — メモリ重い */
/* 案B: 上位16件のみ保持するヒープ式トップリスト — メモリ軽い */
/* 案C: u8 飽和カウント [256][256] = 64KB — 中間 ★推奨 */

#define V86_AH_HIST_MAX 255
static u8 v86_int_ah_hist[256][256];   /* 64KB */
```

V86 セッション終了時に表形式で出力:

```
[INT 18h AH ヒストグラム]
  AH=00h: 121 calls (KB getchar)         [u8最大値で飽和した可能性: yes]
  AH=11h: 127 calls (cursor on)
  AH=14h:  89 calls (1char write)
```

**実装ヒント**:
- 既存 [`kernel/v86.c:37`](../../../kernel/v86.c) `v86_trace[]` を拡張
- `v86_debug.c:387-421` の `write_section_gptrace()` がトレース表示。tick カラムを追加するだけ
- KAPI 追加 `sys_v86_set_trace_filter(int int_no)` で外部制御可
- コード差分: ~50行

**依存**:
- 前提: なし
- 後段: T3.1 (逆アセンブル) — opcode 16B コンテキストを記録するなら entry サイズを再検討

**検証方法**:
1. `v86 -d` で 1024 件採取できることを確認 (旧 128 件で切れていない)
2. `sys_v86_set_trace_filter(0x18)` 設定後に INT 18h 以外がトレースされないことを確認
3. ヒストグラム飽和 (255) ケースで warning が出ること

**留意点**:
- ヒストグラム 64KB はカーネル静的領域として大きい。実装時はビルドフラグで切り離せるようにする
- フィルタ設定はセッション開始前に行う必要あり (実行中変更は競合リスク)

---

### T1.3 BDA スナップショット多段化 + ホスト diff ツール

**対応 §11 項目**: A1 / A2 / D6 / W3 / W10
**目的**: BDA が「いつ」汚染されたかを時系列で特定。NP21/W リファレンスとの差分を自動化。

**仕様**:

#### カーネル側 API

```c
/* kernel/v86_debug.c 拡張 */
void v86_debug_dump_bda_named(const char *tag);
```

呼び出し箇所と tag:

| タイミング | tag | 出力ファイル |
|----------|-----|----------|
| `v86_mem_setup()` 直後 | `init` | `/host/debug/v86_bda_init.bin` |
| IPL 読み込み直後 | `post_ipl` | `/host/debug/v86_bda_post_ipl.bin` |
| 最初の INT 21h 発生時 | `pre_dos` | `/host/debug/v86_bda_pre_dos.bin` |
| ハング検知時 / V86 終了直前 | `exit` | `/host/debug/v86_bda_exit.bin` |

各ファイルは 512 バイト raw バイナリ (BDA 0x00400-0x005FF)。

#### ホスト側ツール `tools/v86_bda_diff.py`

入力:
- `v86_bda_{tag}.bin` (OS32 採取)
- `reference_dos.bin` (T4.1 で採取済み)

出力 (Markdown):

```markdown
# BDA Diff Report — init vs reference_dos

| offset | name          | os32 (init) | dos    | diff | category |
|--------|---------------|-------------|--------|------|----------|
| 0x481  | MEMSIZ        | 00          | 80     | YES  | uninit   |
| 0x484  | CPU_TYPE      | 03          | 03     | -    | match    |
| 0x501  | BIOS_FLAG     | 24          | 27     | YES  | diff     |
| 0x55C  | DISK_EQUIP    | 01          | 01     | -    | match    |
| ...    | ...           | ...         | ...    | ...  | ...      |

統計:
  - 完全一致: 432 / 512
  - OS32=0 / DOS=非0 (uninit候補): 38
  - 両方非0で値違い: 42
```

PC-98 BDA フィールド名は [`kernel/v86_bda.h`](../../../kernel/v86_bda.h) の `BDA_*` 定数定義から自動抽出。

**実装ヒント**:
- 既存 [`kernel/v86_debug.c:732-744`](../../../kernel/v86_debug.c) の BDA dump コードを抽出して関数化
- `vfs_open` + `vfs_write_fd` + `vfs_close` の単純シーケンス
- `paging_is_present()` で teardown 後のページ存在確認必須
- コード差分: kernel側 ~30行 + Pythonスクリプト ~150行

**依存**:
- 前提: なし (T1.1 とは独立)
- 後段: T2.5 (DOS_ref テンプレート) — diff 結果から ref BDA データを生成

**検証方法**:
1. `v86 -d` で 4 つの bin ファイルが生成されることを確認
2. `cmp v86_bda_init.bin v86_bda_exit.bin` で時系列変化を観測
3. `python tools/v86_bda_diff.py v86_bda_exit.bin reference_dos.bin` で markdown 出力

**留意点**:
- バッキング RAM の物理アドレス変換に注意 (`v86_phys_addr` 使用)
- `paging_is_present` を必ず確認 (teardown後はアクセス不能)
- snapshot書込のオーバーヘッドはセッション中の挙動を変えないように

---

### T1.4 BIOS ROM FAR CALL トラッカー

**対応 §11 項目**: A3 / W5
**目的**: IO.SYS が BIOS ROM 内ルーチンを CALL FAR している場合の特定。

**仕様**:

#### 検出対象命令

PC-98 リアルモードで使われる FAR CALL の2形式:

| オペコード | 形式 | 命令長 | 例 |
|----------|------|-------|---|
| `9A oo oo ss ss` | CALL FAR ptr16:16 (直接) | 5B | `CALL FAR F000:0200` |
| `FF /3 ...` | CALL FAR m16:16 (間接 mem) | 2+ B | `CALL FAR [BX]` |

`0xFF /3` の場合: mod r/m バイトの reg = 3 が CALL FAR m16:16。
mod r/m + (potential displacement) のデコードが必要。

```c
/* 0x9A の場合 */
if (opcode == 0x9A) {
    u16 target_off = *(u16 *)(ip_ptr + 1);
    u16 target_seg = *(u16 *)(ip_ptr + 3);
    if (target_seg >= 0xF000) v86_rom_call_record(...);
}

/* 0xFF /3 の場合 */
if (opcode == 0xFF) {
    u8 modrm = ip_ptr[1];
    if (((modrm >> 3) & 0x07) == 3) {  /* /3 = CALL FAR m16:16 */
        u8 *mem = v86_decode_modrm(regs, modrm, ip_ptr + 2);
        u16 target_off = *(u16 *)mem;
        u16 target_seg = *(u16 *)(mem + 2);
        if (target_seg >= 0xF000) v86_rom_call_record(...);
    }
}
```

#### ROM CALL リングバッファ

```c
struct v86_rom_call_entry {
    u32 tick;
    u16 caller_cs, caller_ip;
    u16 target_cs, target_ip;
    u16 ax_at_call;
    u16 bx_at_call;
};  /* 16B */

#define V86_ROM_CALL_SIZE 32
static struct v86_rom_call_entry rom_calls[V86_ROM_CALL_SIZE];
static u32 rom_call_idx = 0;
static u32 rom_call_count = 0;
```

V86 終了時、CS が F000 以上の場合は **「リセットベクタ到達」と「ROM ルーチン途中」を区別** してログ出力:
- IP=0x0000 → reset vector
- その他 → 専用 ROM TRACE セクション

**実装ヒント**:
- GP ハンドラ ([`kernel/v86.c`](../../../kernel/v86.c)) の opcode switch に追加
- mod r/m デコードは既存の I/O 命令デコードに似た構造 → ヘルパ関数化
- T1.1 のイベントとして `V86_EV_ROM_CALL` (critical) で記録すると即時 flush される
- コード差分: ~80行

**依存**:
- 前提: T1.1 (critical event の即時 flush 機構) — なくても動作するが連携推奨
- 後段: T3.1 (逆アセンブル) — ROM 内ルーチンの中身解析時に使用

**検証方法**:
1. テスト用 COM プログラムで `CALL FAR F000:0200` を実行 → ROM CALL リングに記録されるか
2. NP21/W で DOS5 ブート → V86 終了時に CS=F0xx で停止した場合、ROM CALL ログが出力されるか
3. IP=0 のリセットベクタ到達と区別表示されるか

**留意点**:
- ROM内 I/O 命令も #GP するため、ROM 内部ループの分析は別途必要 (T3.1 逆アセンブルと併用)
- 間接 CALL (`0xFF /3`) のメモリオペランド読み出しは V86 メモリ空間へのアクセス → `v86_phys_addr` 経由必須

---

## 13.3 Tier 2 — 検証カバレッジ拡張 (5件)

### T2.1 MEMSW スナップショット

**対応 §11 項目**: A5 / W4
**目的**: TVRAM 領域のメモリスイッチ (0xA0000:3FE2-3FF7) の設定/上書きを観測する。

**仕様**:

```c
void v86_debug_dump_memsw_named(const char *tag);
```

範囲: `0xA0000:0x3FE2 - 0x3FF7` (22バイト)
出力: ログファイル内に `[MEMSW @{tag}]` セクション。`init` / `exit` の2回採取し差分も表示

#### MEMSW フィールド意味

| アドレス | フィールド | 意味 |
|---------|-----------|------|
| 0xA3FE2 | MEMSW1 | bit6=25行モード, bit3=RS232C |
| 0xA3FE4 | MEMSW2 | クロック系 (10MHz/8MHz) |
| 0xA3FE6 | MEMSW3 | bit2=31kHz CRT, bit7=ディップスイッチ |
| 0xA3FE8 | MEMSW4 | 予約 |
| 0xA3FEA | MEMSW5 | 予約 |
| 0xA3FEC | MEMSW6 | 予約 |
| 0xA3FEE-0xA3FF7 | 拡張 | 機種固有 |

出力例:

```
[MEMSW @init]
  3FE2: 48 (MEMSW1: 25行+RS232C)
  3FE4: 01 (MEMSW2: 10MHz)
  3FE6: 04 (MEMSW3: 31kHz)
  3FE8: 00 (MEMSW4: ―)
  ...

[MEMSW @exit]
  3FE2: 48 (MEMSW1: 25行+RS232C)
  3FE4: 01 (MEMSW2: 10MHz)
  3FE6: 04 (MEMSW3: 31kHz)
  3FE8: 00 (MEMSW4: ―)
  ...

[MEMSW diff]
  no changes
```

**実装ヒント**:
- TVRAM領域は常にマップされており `paging_is_present` 不要
- WORD単位アクセス (偶数アドレスのみ意味あり)
- コード差分: ~40行

**依存**:
- 前提: なし
- 後段: T1.3 (BDA スナップショット) と同タイミングで採取するのが効率的

**検証方法**:
1. `v86 -d` で v86_diag.log に MEMSW セクションが含まれるか
2. ゲストが MEMSW を書き換えるテストケース (簡易 COM) で diff が出るか

**留意点**:
- TVRAM領域なのでページ存在は保証されているが、書き込み権限の確認は念のため必要

---

### T2.2 I/O ポート分類タグ付き統計

**対応 §11 項目**: D1 / W6
**目的**: 「未分類で実HW直送」のポートを識別し、リスク評価する。

**仕様**:

```c
/* 旧 v86_io_stats[16] → 拡張 */
struct v86_io_stat {
    u16 port;
    u32 read_count;
    u32 write_count;
    u8  classification; /* 1=passthrough, 2=virt, 3=protected, 4=fallthrough */
};

#define V86_IO_STAT_SIZE 64        /* 旧 16 → 64 */
static struct v86_io_stat v86_io_stats[V86_IO_STAT_SIZE];
```

`classification` の判定は v86.c のディスパッチ内で実施:
- PIC/PIT/FDC/DMA/VSYNC ハンドラが処理 → `virt`
- `v86_io_is_protected()` → `protected`
- IOビットマップ allow リスト内 → `passthrough` (実際は GP しないので統計に乗らないが、明示的に list 化)
- いずれにも該当せず実HW直送 → `fallthrough` ← **要分析**

出力例:

```
Port  R-Cnt  W-Cnt  Class
----  -----  -----  -----
0041   1234      0  virt
0188      0   9012  passthrough
005C      5      0  fallthrough ← 警戒
```

**実装ヒント**:
- `v86_io_stat_record()` ([`kernel/v86.c:158`](../../../kernel/v86.c)) の引数に classification を追加
- v86_debug.c の I/O ポート出力セクションを拡張
- コード差分: ~30行

**依存**:
- 前提: なし
- 後段: T3.3 (DMA転送ログ) — fallthrough ポートに DMA関連があれば対象に追加

**検証方法**:
1. 既知の virt ポート (0x00 PIC等) が virt 分類で記録されるか
2. 既知の passthrough ポート (0x188 FM音源) が passthrough 分類か
3. `fallthrough` ポートが0件か少数であること (もしくは説明可能なもの)

**留意点**:
- 既存統計データ構造の変更は API 互換性に影響 → 旧名関数を残しつつ拡張版を新設

---

### T2.3 IVT 差分検出

**対応 §11 項目**: D2 / D3 / D6 / W7
**目的**: ゲストがフックした INT vector を特定する。同時に D3 (ダミーIVT 不整合) を解消。

**仕様**:

```c
/* 初期 IVT スナップショット */
static u32 ivt_snapshot[256];

void v86_debug_snapshot_ivt_init(void);  /* v86_mem_setup() 末尾で呼出 */
void v86_debug_dump_ivt_diff(void);      /* V86 終了時に呼出 */
```

出力例:

```
[IVT diff]
 INT 08h: 003F:0000 (dummy) → 1F30:01D3  (Ys music driver?)
 INT 09h: 003F:0000 (dummy) → 1F30:0250
 INT 1Bh: 003F:0000 (dummy) → unchanged (kernel hooks ignored)
```

#### 同時に D3 を解消

ダミーIVT 値 (`0x003F:0x0000`) と現行コードの `V86_IS_DUMMY_IVT` マクロを参照 → 不整合修正:
- [`v86_debug.c:472`](../../../kernel/v86_debug.c) と `:755` の `seg == 0x0050` チェックを `seg == V86_DUMMY_IVT_SEG` に修正
- [`v86.c:646`](../../../kernel/v86.c) のコメント `0x0050:0x0000` を `0x003F:0x0000` に修正

**実装ヒント**:
- 1KB の静的バッファ追加 + memcpy 1回 + diff ループ 256回
- コード差分: ~60行

**依存**:
- 前提: なし
- 後段: T2.4 (ウォッチポイント) — IVT書き換え位置の特定に使える

**検証方法**:
1. ゲスト COM で IVT[0x80] を書き換え → diff に出るか
2. 既存の `v86_debug.c:472, :755` の seg比較が `V86_DUMMY_IVT_SEG` 経由になっているか

**留意点**:
- 既存ダミーIVT 値 (0x003F:0x0000) はマクロ `V86_IS_DUMMY_IVT` 経由で参照 ([`v86.h:142-145`](../../../kernel/v86.h))

---

### T2.4 メモリウォッチポイント

**対応 §11 項目**: A2 / D6
**目的**: 「BDA の特定オフセットが書き換わる瞬間」を捕捉。

**仕様**:

#### API

```c
void v86_dbg_watch_set(u32 addr, u32 size);  /* size=1〜4 */
void v86_dbg_watch_clear(u32 addr);
```

#### 実装シーケンス (PTE NOT_PRESENT 方式)

```
1. ウォッチ設定時:
   - 該当アドレスを含む 4KB ページを PTE NOT_PRESENT に
   - watch_table[] にアドレス + size + コールバック登録

2. ゲストがウォッチアドレスにアクセス → #PF 発生:
   - #PF ハンドラで watch_table をチェック
   - マッチ → tick / CS:IP / 方向(R/W) / 書込時の値を記録
   - PTE を一時的に PRESENT に戻す
   - EFLAGS.TF=1 セットで命令再実行 → 1命令後 #DB
   - #DB ハンドラで PTE を再度 NOT_PRESENT に
   - TF クリアして V86 復帰

3. 別ページへの通常アクセスは PTE PRESENT のままで #PF せず
```

#### 記録構造

```c
struct v86_watch_entry {
    u32 tick;
    u32 addr;
    u16 cs;
    u16 ip;
    u8  dir;       /* 0=R, 1=W */
    u8  size;      /* 1/2/4 */
    u32 value;     /* W時のみ有効 */
};
```

**実装ヒント**:
- 既存 `#PF` ハンドラ ([`kernel/isr_handlers.c`](../../../kernel/isr_handlers.c)) の拡張
- EFLAGS.TF + #DB ハンドラ (TF経由のシングルステップ)
- T3.4 (シングルステップ) と機構が一部共通
- コード差分: ~150行 (中規模)

**依存**:
- 前提: T3.4 (TF/#DBハンドラ基盤) があると流用しやすい
- 後段: なし

**検証方法**:
1. `v86_dbg_watch_set(0x00500, 1)` で 0x500 へのアクセスを記録できるか
2. 高頻度アクセスでも #PF → #DB → 復帰のループが正しく動くか
3. 通常アクセス (他ページ) のパフォーマンス劣化が許容範囲か

**留意点**:
- 高負荷 (1回のアクセスにつき2回 #PF + 1回 #DB) → 通常は OFF にしておく
- DOS5 IPL 中のみ ON にする等、条件付き有効化が望ましい (T3.5 と連携)
- 4KB ページ単位でしか保護できないため、隣接アドレスへの誤検知を許容

---

### T2.5 MS-DOS 期待値テンプレート埋め込み

**対応 §11 項目**: A1 / A2 / A6 / W10
**前提**: NP21/W で MS-DOS 6.2 (NEC版) を直接ブート → BDA を [T4.1 手順案1'](#t41-np21w-リファレンス-bda-採取手順) で採取済み

**仕様**:

```c
/* kernel/v86_dos_ref.c (新規) */
const u8 v86_dos_ref_bda[512] = {
    /* 0x400: ... */ 0x00, 0x00, ...,
    /* 0x481: MEMSIZ */ 0x80, 0x02, /* 640 KB */
    ...
};
/* マスク (0xFF=比較対象, 0x00=無視 / DOS版差や時刻依存値を除外) */
const u8 v86_dos_ref_bda_mask[512] = { ... };
```

#### カーネル側 diff API

```c
void v86_debug_compare_bda_to_ref(void);
/* 起動時にカーネル側で diff レポートを直接出力可能 */
```

#### 名称設計

- DOS5 専用ではなく汎用に: `v86_dos_ref_bda` (DOS6.2 採取値が DOS5 にもほぼ流用可)
- 拡張性のため `freedos_ref_bda` / `dos3_ref_bda` も用意可能な API 設計に

**実装ヒント**:
- 512B + 512B = 1KB の const テーブル
- diff ロジックは T4.3 (Python版) と同じ判定を C で再実装
- コード差分: kernel側 ~100行 + データ 1KB

**依存**:
- 前提: T4.1 (リファレンス BDA 採取手順) で `reference_dos.bin` 確保
- 後段: なし

**検証方法**:
1. リファレンス採取後、`kernel/v86_dos_ref.c` を生成するスクリプト (`tools/gen_dos_ref_c.py`) で C 配列に変換できるか
2. V86 起動時にカーネルが期待値との diff レポートを出力するか

**留意点**:
- DOS5 と DOS6.2 で BDA に **差異がありうるフィールド** はマスクで除外:
  - BIOS バージョン領域 (0x0598h-)
  - 時刻依存フィールド (RTC作業領域)
- マスク作成は最初の差分レポート (A1 検証) を見てから判断
- リファレンス採取の手作業は一度きり (採取手順は §T4.1 参照)

---

## 13.4 Tier 3 — 深部解析・将来用 (6件)

### T3.1 逆アセンブル支援

**対応 §11 項目**: A4 / W8
**目的**: GP TRACE エントリの opcode バイト列を可読命令文字列に変換する。

**仕様**:

```c
/* kernel/v86_disasm.c (新規) */
int v86_disasm_one(const u8 *bytes, char *out, size_t outsz);
/* 戻り値: 消費バイト数 */
```

#### デコード対象命令一覧

最小デコーダ (フル x86 デコーダではない):

| カテゴリ | 命令 |
|---------|------|
| INT 系 | INT n / IRET / IRETD |
| CALL 系 | CALL near / CALL FAR direct / CALL FAR indirect / RET / RETF |
| I/O 系 | IN imm8/DX / OUT imm8/DX / INSB/W / OUTSB/W |
| 制御フラグ | CLI / STI / HLT |
| ジャンプ | JMP near / JMP FAR / Jcc |
| MOV 即値 | MOV r8/r16, imm |
| PUSH/POP | PUSH r / POP r / PUSHF / POPF |

#### 出力例

```
1F30:0123  CD 18           INT 18h
1F30:0125  9A 00 02 00 F0  CALL FAR F000:0200
1F30:012A  EE              OUT DX, AL
```

GP TRACE エントリに opcode 16B コンテキストを記録する場合、`v86_trace_entry` を更に拡張する必要あり (現在の 16B → 32B)。

**実装ヒント**:
- 既存 GP ハンドラ ([`kernel/v86.c`](../../../kernel/v86.c)) 内のデコードロジックを抽出
- 完全な x86 デコーダではなく、トレース必要な命令種別のみで十分
- コード差分: ~250行 (大規模)

**依存**:
- 前提: T1.2 (GP TRACE 拡張) で opcode コンテキスト記録できると効果大
- 後段: T1.4 (ROM CALL) ROM 内ルーチン解析、T3.4 (シングルステップ) 出力

**検証方法**:
1. テスト用 COM の各命令が正しく逆アセンブルされるか
2. 不明オペコード時に「unknown opcode XX」を返すか
3. 命令長計算が正しいか (誤ると後続命令も誤デコード)

**留意点**:
- 完全な x86 デコーダではなく、トレース必要な命令種別のみで十分
- 既存の GP ハンドラ内デコード ロジックを再利用できる可能性

---

### T3.2 PIC EOI シーケンス検証

**対応 §11 項目**: D6 (派生)
**目的**: ISR ビットが立っていない状態での EOI 発行 (=異常) を検出。

**仕様**:

```c
struct v86_eoi_entry {
    u32 tick;
    u8  which;     /* 0=master, 1=slave */
    u8  cleared_bit;  /* 0-7 = clear した IRQ ビット, 0xFF=何もクリアしなかった */
    u8  isr_before;
    u8  isr_after;
};

#define V86_EOI_LOG_SIZE 64
static struct v86_eoi_entry eoi_log[V86_EOI_LOG_SIZE];
```

`v86_pic.c` の EOI 処理 (OUT 0x00/0x08 → 0x20) で履歴を記録:

```c
static void v86_pic_eoi(int which) {
    u8 isr_before = vpic[which].isr;
    /* 既存の最上位ビットクリアロジック */
    int bit;
    int cleared = -1;
    for (bit = 0; bit < 8; bit++) {
        if (vpic[which].isr & (1 << bit)) {
            vpic[which].isr &= ~(1 << bit);
            cleared = bit;
            break;
        }
    }
    v86_eoi_log_record(which, cleared, isr_before, vpic[which].isr);
}
```

出力例:

```
[PIC EOI sequence]
tick=1234 master EOI cleared IRQ0 (ISR 01→00) — OK
tick=1245 master EOI cleared NONE (ISR 00→00) — WARN orphan EOI
```

**実装ヒント**:
- 既存 [`kernel/v86_pic.c`](../../../kernel/v86_pic.c) の EOI 処理ポイントに記録呼出を追加
- リングバッファは T1.4 ROM CALL と同パターン
- コード差分: ~50行

**依存**:
- 前提: なし
- 後段: なし (独立性高)

**検証方法**:
1. 正常な IRQ → EOI シーケンスで全て OK が記録されるか
2. ゲストが意図的に多重 EOI を発行 → orphan が記録されるか

**留意点**:
- DOS5 の PIC 初期化処理が ICW1-4 シーケンスを完了する前の OUT は別カテゴリ扱い

---

### T3.3 DMA 転送ログ

**対応 §11 項目**: D1 / B1 (HDD 拡張時)
**目的**: DMA 転送 (FDC/HDD バックエンド) の生データを観測。

**仕様**:

```c
struct v86_dma_entry {
    u32 tick;
    u8  ch;
    u8  mode;
    u32 phys_addr;
    u16 count;
    u8  trigger;   /* 1=FDC READ, 2=FDC WRITE, 3=HDD (将来) */
};

#define V86_DMA_LOG_SIZE 32
static struct v86_dma_entry dma_log[V86_DMA_LOG_SIZE];
```

`v86_dma.c` で DMA 起動 (FDC実行直前 etc.) を検出し、転送1回ごとに記録。

**実装ヒント**:
- 既存 [`kernel/v86_dma.c`](../../../kernel/v86_dma.c) の `v86_dma_get_transfer()` 呼出位置にフック
- コード差分: ~40行

**依存**:
- 前提: なし
- 後段: B1 (HDDブート) — DMA経路の検証で必須

**検証方法**:
1. FDD ブート時に DMA cycle が記録されるか
2. addr / count が `v86_phys_addr` の結果と整合するか

---

### T3.4 シングルステップ (TF) モード

**対応 §11 項目**: A4 深掘り
**目的**: 命令単位の極詳細トレース (TF フラグ経由)。

**仕様**:

```c
extern int v86_singlestep_enabled;  /* 0=OFF, 1=ON */
extern u32 v86_singlestep_max_count; /* 上限 (0=無制限) */
```

V86 EFLAGS に TF (Trap Flag, bit8) を立てた状態で V86 に入る → 1命令ごとに #DB 例外。

#### TF フラグ管理

```
1. v86_enter() 前: EFLAGS |= (1 << 8)  /* TF セット */
2. ゲスト命令実行 → 1命令後 #DB 発火
3. #DB ハンドラ:
   - GP TRACE と同等の記録
   - カウンタ ++、上限到達なら TF クリアして通常モード
   - そうでなければ EFLAGS.TF=1 を維持して V86 復帰
4. v86_singlestep_enabled=0 で TF クリア
```

#### 制御

- `-dss` (debug single-step) オプションで有効化
- 1〜10秒の短時間にのみ ON 推奨 (極端に低速、ログ膨大)

**実装ヒント**:
- 既存 ISR スタブの #DB エントリ追加 (現状未対応?)
- EFLAGS 操作は `v86_enter.asm` の IRETD 前で実施
- コード差分: ~100行

**依存**:
- 前提: なし
- 後段: T2.4 (ウォッチポイント) — TF/#DB機構を共有

**検証方法**:
1. テスト COM で `-dss` 起動 → ログ件数が命令数と一致するか
2. TF クリア後に通常速度に戻るか

**留意点**:
- TF を立てるだけで命令ごとに割り込み発火するため、ゲストの動作タイミングが完全に崩れる
- リアルタイム性が必要な箇所では使えない (タイマ割り込みとの整合性)
- IRQ ハンドリングも TF の対象になるため、IRQ ハンドラ内では一時的に TF クリア推奨

---

### T3.5 条件付きトレース開始

**対応 §11 項目**: A4
**目的**: 前段ノイズを除去し、ハング直前のみのトレースを取得。

**仕様**:

```c
enum v86_trace_trigger_kind {
    V86_TT_INT,        /* 特定 INT 発行 */
    V86_TT_CS_IP,      /* 特定 CS:IP 到達 */
    V86_TT_TICK,       /* 指定 tick 到達 */
};

void v86_trace_trigger_set(enum v86_trace_trigger_kind kind,
                           u32 arg1, u32 arg2);
```

#### トリガ条件

| トリガ | コマンドライン例 | 動作 |
|--------|----------------|------|
| INT発行 | `-dt-int=21` | INT 21h 発行を検出したら GP TRACE ON |
| CS:IP到達 | `-dt-cs=1F30:0123` | 該当アドレス実行を検出したら ON |
| Tick到達 | `-dt-tick=500` | tick_count=500 で ON |

トリガ前は GP TRACE OFF、トリガ後 ON。ハング検知の前段「ノイズ」を消し、ハング直前の動作だけを取れる。

**実装ヒント**:
- GP TRACE 記録関数の先頭にトリガチェックを追加
- KAPI 経由でユーザーランドから設定可能に
- コード差分: ~50行

**依存**:
- 前提: T1.2 (GP TRACE 拡張) — フィルタ機構と統合
- 後段: なし

**検証方法**:
1. `-dt-int=21` 設定 → INT 21h 発行前の trace が空、発行後の trace が記録されるか

**留意点**:
- トリガ条件は1つのみ (複合条件は将来課題)

---

### T3.6 シリアル ライブ ストリーム (フォールバック)

**対応 §11 項目**: W1 (HostDrv 不在時)
**位置づけ**: 旧 T1.1 から降格。通常運用では T1.1 (HostDrv) を使う。

**用途**:
- HostDrv 初期化前 (boot 最初期) のデバッグ
- HostDrv ポート保護違反の調査時
- 将来の実機検証等で HostDrv が使えない環境

**仕様**:
- `v86_event_record()` 内で `v86_debug_stream_enabled` が立っていれば、追加で RS-232C へ1行テキストを送出
- フォーマット例: `G:1F30:0123:CD:18:0A:01` (kind:CS:IP:Op:INT:AH:AL)
- 接頭辞 `<<V86>>` で通常シェル出力と区別
- バッファリングなし (低レイテンシ優先)

**制約**:
- 9600bps 想定では 50行/秒程度が上限 → 高頻度イベントは取りこぼす
- 19200bps 以上で運用するか、レート制限を組み合わせる

**起動**:
- `-ds` オプション (debug serial stream) で有効化
- T1.1 と併用も可能 (ただし通常は冗長)

**実装ヒント**:
- 既存 `serial_putchar` を1イベントあたり ~20回呼ぶ
- コード差分: ~30行

**依存**:
- 前提: T1.1 の `v86_event_record()` 機構
- 後段: なし

**検証方法**:
1. `-ds` 起動 → ホスト側 rshell で `<<V86>>` 行が受信できるか
2. T1.1 と併用時に二重出力されないか

---

## 13.5 Tier 4 — ホスト側ツール (3件)

### T4.1 NP21/W リファレンス BDA 採取手順

**対応 §11 項目**: A1 / T2.5 の前提

#### レイヤ構造の整理

```
[ Windows + NP21/W ]   ← 外側のホスト (実機相当・改変不可)
       ↓ エミュレート
[ PC-98 ハードウェア ]
       ↓ ブート対象 (排他)
   ┌───┴────┐
[ OS32 ]   [ MS-DOS6.2 ]   ← どちらか1つしか NP21/W で起動できない
   ↓ V86モード
[ DOS5 ] ← ★現在ハング中★
```

OS32 は V86 ゲストの BDA をバッキングRAMから直接読めるが、**正常動作する MS-DOS の BDA** が欲しい場面ではそれが取れない (V86 内 DOS はハング中)。そのため NP21/W を一度 OS32 から **MS-DOS 直接ブート** に切替える必要がある。NP21/W にはステートセーブも外部メモリインスペクタも (基本的に) 無いため、DOS 自身に BDA を export させる手段を取る。

#### リファレンス環境

- **MS-DOS 6.2 (NEC PC-98版)** — DEBUG.COM が標準で含まれる確認済み環境 (2026-05)
- 採取される BDA は **DOS5.0A IO.SYS が要求する値と概ね一致** すると見込まれる (PC-98 BIOS/BDA 仕様は MS-DOS バージョン間でほぼ共通)
- DOS5 固有の追加フィールドがあれば §11.2 A2 の調査で別途特定

#### 手順案 1' — DEBUG.COM + ファイルリダイレクト (★第一推奨)

```
NP21/W で DOS6.2 起動 → A:\> プロンプト

A:\> DEBUG > A:\BDA.TXT
-D 0:400 L 200
-Q

NP21/W を停止 → FDD イメージをアンマウント
ホスト側:
  $ mtype -i fd_dos62.fdi ::BDA.TXT > captured.txt
  $ python tools/v86_dos_bda_parse.py captured.txt > reference_dos.bin
```

**所要**: ~3分。手作業最小・再現可能性高。
**前提**: `mtools` (Linuxホスト) または同等の FAT12 マウントツール。

#### 手順案 2' — BDADUMP.COM + RS-232C (自動化向き・併記)

NP21/W が rshell シリアル (port 0x30/0x32) でホストに繋がっている場合、DOS から直接シリアルへ書き出せば手作業ゼロで完結する。

1. 自作 16 ビット COM プログラム `BDADUMP.COM` (TASM/JWASM 等で 64B 程度) を作成
   - BDA 0x0400-0x05FF を 8251 経由で port 0x30 へ byte 送信
   - 終端マーカー (例: `0xFF 0xFF 0xFF 0xFF`) 付与
2. ホスト側で rshell シリアルを raw bytes として受信、終端マーカーで切り出し
3. `reference_dos.bin` を直接生成

**所要**: 初期セットアップ後は ~30秒。

**留意点**:
- 8251 ポート (0x30/0x32) は OS32 では保護ポートだが、DOS6.2 直接ブートでは NP21/W が rshell 用に提供する 8251 にそのまま書き込める
- ボーレート設定が必要 (デフォルト 9600bps 想定。NP21/W 設定と整合)

#### 手順案 3 — IO.SYS / FreeDOS kernel 改造 (上級・参考)

FreeDOS(98) kernel.sys の `init_kernel` 末尾に BDA シリアル送信コードを追加。ただし MS-DOS 純正 IO.SYS のソースは存在しないため、**FreeDOS BDA のリファレンス** にはなっても **MS-DOS BDA のリファレンス** にはならない。比較対象として補助的に使う。

#### 手順案 4 (旧案1) — DEBUG.COM 画面キャプチャ (非推奨)

`DEBUG -D 0:400 L 200` の画面 hex 出力を NP21/W テキストコピー機能で取得する案。手作業が多くミスが入りやすいため、手順案1' が使えない時の **最終手段** に降格。

---

→ **第一推奨: 案1' (DEBUG.COM + ファイルリダイレクト)**。
→ 自動化したいなら **案2' (BDADUMP.COM + RS-232C)**。

**ホスト側パーサ** (`tools/v86_dos_bda_parse.py`):
- DEBUG.COM の `D` コマンド出力 (16進ダンプ ASCII) を入力
- 512バイト raw バイナリに変換
- BDA フィールド名を `kernel/v86_bda.h` から取得し、注釈付きの中間 markdown も出力
- 案2' のシリアル受信 raw バイナリも処理できるよう兼用設計

**依存**:
- 前提: なし (ドキュメント整備のみ)
- 後段: T2.5 / T4.3 — 採取した `reference_dos.bin` を入力に取る

**検証方法**:
1. DOS6.2 で `D 0:400 L 200` 実行 → A:\BDA.TXT 生成確認
2. mtools 抽出 → `wc -c reference_dos.bin` で 512 バイト
3. 16進ダンプして既知の MEMSIZ 値 (0x281) が見えるか確認

---

### T4.2 V86 ログビューワ

**対応 §11 項目**: W9
**目的**: テキストログを目視で読まずに済むよう、可視化・要約する。

**仕様** (`tools/v86_log_viewer.py`):

入力: `/host/debug/v86_diag.log` + `/host/debug/v86_events.log`

#### 機能

1. **タイムライン**: tick 軸で GP / INT / IRQ をプロット
   - 出力: matplotlib PNG
   - 行: tick (横軸) / kind (縦カテゴリ)
2. **INT ヒストグラム**: AH 別の上位20件 (要 T1.2 拡張ログがあれば)
   - 出力: テキスト表 or HTML 棒グラフ
3. **BDA きれい表示**: フィールド名注釈付き hex ダンプ
   - 入力: `/host/debug/v86_bda_*.bin`
   - 出力: Markdown 表
4. **GP TRACE のフィルタ表示**: INT 18h のみ、CS範囲 etc.
   - 出力: stdout テキスト

#### コマンドライン例

```
$ python tools/v86_log_viewer.py --timeline > timeline.png
$ python tools/v86_log_viewer.py --histogram --int 0x18
$ python tools/v86_log_viewer.py --bda v86_bda_exit.bin > bda.md
$ python tools/v86_log_viewer.py --trace --filter-int 0x18 | less
```

**実装ヒント**:
- 標準ライブラリ + matplotlib (optional)
- 16B 固定長構造体は `struct.unpack` で簡単に読める
- スクリプト規模: ~300行

**依存**:
- 前提: T1.1 (v86_events.log) / T1.2 (拡張trace) / T1.3 (BDA dump)
- 後段: なし

**検証方法**:
1. サンプルログで各機能が動作するか
2. 出力が ASCII (テキスト) でも文字化けしないか

---

### T4.3 BDA diff レポート生成

**対応 §11 項目**: A1 / W10
**目的**: OS32 採取の BDA と DOS6.2 リファレンスを比較し、Markdown レポートを生成。

**仕様** (`tools/v86_bda_diff.py`):

入力:
- `/host/debug/v86_bda_*.bin` (T1.3 の OS32 採取)
- `reference_dos.bin` (T4.1 で採取)

出力 (Markdown):

```markdown
# BDA Diff Report

入力:
- OS32: v86_bda_exit.bin (512B)
- Reference: reference_dos.bin (512B)

統計:
- 完全一致: 432 / 512 (84.4%)
- OS32=0 / DOS=非0 (uninit候補): 38
- 両方非0で値違い: 42

## 差分詳細 (50件)

| offset | name           | os32 | dos    | diff | category |
|--------|----------------|------|--------|------|----------|
| 0x481  | MEMSIZ         | 00   | 80     | YES  | uninit   |
| 0x484  | CPU_TYPE       | 03   | 03     | -    | match    |
| 0x501  | BIOS_FLAG      | 24   | 27     | YES  | diff     |
| 0x55C  | DISK_EQUIP     | 01   | 01     | -    | match    |
| 0x564  | FDC_RESULT(ST0)| 00   | 00     | -    | match    |
| 0x582  | BOOT_ID        | 00   | 06     | YES  | uninit   |
| 0x584  | BOOT_DEV       | 90   | 90     | -    | match    |
| 0x598  | BIOS_VER[0]    | 00   | 4D     | (mask)| ignored |
| ...    | ...            | ...  | ...    | ...  | ...      |
```

#### コマンドライン

```
$ python tools/v86_bda_diff.py v86_bda_exit.bin reference_dos.bin > report.md
$ python tools/v86_bda_diff.py --no-match v86_bda_init.bin reference_dos.bin
```

#### マスク対応

`tools/dos_bda_mask.json` で除外領域を指定:

```json
{
  "ignored_ranges": [
    {"start": "0x598", "end": "0x5BF", "reason": "BIOS version (DOS版で異なる)"},
    {"start": "0x470", "end": "0x47F", "reason": "RTC time (時刻依存)"}
  ]
}
```

**実装ヒント**:
- `kernel/v86_bda.h` の `#define BDA_*` を正規表現で抽出してフィールド名辞書を構築
- スクリプト規模: ~200行

**依存**:
- 前提: T4.1 (リファレンス採取) / T1.3 (OS32 採取)
- 後段: T2.5 (カーネル側 const テーブル化のための入力データ生成)

**検証方法**:
1. 既知の差分 (例: MEMSIZ) が正しく検出されるか
2. マスク領域が ignored カテゴリで表示されるか
3. 統計値が手計算と一致するか

---

## 13.6 関連ドキュメント

| ドキュメント | 関係 |
|------------|------|
| [12_debug_tools_inventory.md](12_debug_tools_inventory.md) | 既存機能棚卸し (本ドキュメントの前提) |
| [14_debug_tools_roadmap.md](14_debug_tools_roadmap.md) | 実装ロードマップ・メモリ消費・KAPI |
| [11_gaps_and_verification.md](11_gaps_and_verification.md) | 上流の検証項目 (A1-A6 等) |
| [09_msdos_roadmap.md §9.2](09_msdos_roadmap.md) | 検証Step 1/2/3 の手順詳細 (ツール仕様の上流) |
| [02_cpu_emulation.md §2.6](02_cpu_emulation.md) | 既存 GP TRACE 仕様 |
| [03_io_port_map.md §3.6](03_io_port_map.md) | I/O 統計の現状 |
| [`kernel/v86_debug.c`](../../../kernel/v86_debug.c) | 拡張対象本体 |
| [`kernel/v86.h:88-110`](../../../kernel/v86.h) | 既存カウンタ extern 宣言 |
| [`kernel/v86_disk.h:78`](../../../kernel/v86_disk.h) | 既存 disk_log_entry |
