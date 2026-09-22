# TASK_HAL_WIRING — 結線の土台 (割り込みの動的登録 / 8237 DMA の共通部 / DMA プール / PCI の結線表 / µs 時計)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 v2 (Codex 設計レビュー往復 1 の 14 件を反映、往復 2 待ち)**。
> ユーザー指示 2026-09-23: 「結線の土台の票の設計を先に起こす」。

正典の関係: [`PLAN.md`](PLAN.md) §3-1 (HAL の棚卸し)、§3 (ドライバの動的読み込み — この票の「取り決め」を後で外部モジュールに開く)、
[`../realhw/TASK_LAN_82557.md`](../realhw/TASK_LAN_82557.md) (L-B 82557 が最初の顧客)、§5-5 (PCM リングとタイマが 2 番目の顧客)。

## 0. なぜ要るか

PCI の装置は **BIOS が IRQ を割り当て、複数の装置が 1 本の IRQ を共有し得る**。いまの OS32 は `kernel/isr_stub.asm` が
装置ごとに固定の C ハンドラを `extern` で結んでおり (`timer_handler` / `kbd_irq_handler` / `serial_irq_handler` /
`fdc_irq_handler` / `mouse_irq_handler` / `ne2k_irq` (`irq_stub_nic_3/5/6`)、それ以外は `IRQ_UNEXP` で EOI だけ)、
**起動時に読んだ IRQ 番号でハンドラを結ぶ口が無い**。DMA も `dma_setup()` が `drivers/fdc.c` の中で ch2 決め打ち、
DMA に使えるメモリは静的配列 + 整列で取っている。82557 (バスマスタ、IRQ は BIOS 次第) と CS4231 (8237 の #1/#3) を
載せるには、この 4 つを装置から切り離した層にする必要がある。

**v2 で変えたこと (Codex 往復 1)**: 共有 IRQ は「全員を呼んで集約 + 有界の再走査」、EOI の責任を 1 か所に、
登録の契約 (対応 IRQ・所有・寿命)、DMA メモリは pgalloc ではなく **全 PD で見える固定プール** (SHM 帯から切る)、
`dma_chan_remaining` の安定読み、one-shot / cyclic の区別、0439h の 3 値、BAR=0 の除外は driver 側へ、
µs 時計は **PIT の分周が 1.9968MHz 決め打ち (既存バグ) の修正**を前提に整数演算で、KAPI は u32 の 2 分割で返す。

## 1. 設計

### 1-0. 前提の修正: PIT の分周をクロック判定に合わせる (既存バグ)

`kernel/idt.c` の `pit_init()` は `PIT_CLOCK` = 1,996,800Hz 決め打ちで分周する。**2.4576MHz 系 (実機 Ra266 は
`0000:0501h` bit7 = 0)** では 100Hz を頼んでも **tick = 8.125ms (123Hz)** になっており、`tick_count` を使う待ち・番犬・
校正すべてが 23% 速い。`serial_detect_clock()` は `pit_init` より**後** (kernel.c 194 → 228) に呼ばれている。
→ クロック判定を `sysclk_detect()` として `serial.c` から切り出し、`pit_init` の**前**に呼ぶ。`pit_init(hz)` は
検出したクロックで分周し、**実際の周期 (µs) と再ロード値を `struct pit_setup` に記録**する (1-5 が読む)。
NP21/W (1.9968MHz) では何も変わらない。**実機では時間の全部が正しくなる**ので、この票の最初の着地単位にする。

### 1-1. 割り込みの動的登録 — `kernel/irq.c` / `kernel/irq.h`

```c
typedef int (*irq_handler_fn)(unsigned int irq, void *arg);   /* 1 = 自分の要因を見つけて落とした / 0 = 自分のではない */
int  irq_register(unsigned int irq, irq_handler_fn fn, void *arg, unsigned int flags);
int  irq_unregister(unsigned int irq, irq_handler_fn fn, void *arg);
#define IRQ_F_SHARED  0x01
```

**対応 IRQ (契約)**: 動的登録を受けるのは**共通スタブ `irq_stub_common %1` に結ばれた IRQ だけ**。この票で共通スタブに
するのは、いま `IRQ_UNEXP` の 3/5/6/8/9/10/14/15 と、`irq_stub_nic_3/5/6` (LGY-98) の 3/5/6。
それ以外 (0 タイマ / 1 KBD / 4 シリアル / 7 カスケード / 11 FDC / 12 マウス) は固定スタブのままで、
`irq_register` は **`-ENOTSUP` (負) で拒否**する。範囲外 (≥16) も拒否。呼び手 (82557) は拒否されたら装置側の
割り込みを止めてポーリングへ落とす。LGY-98 は `lgy98_init()` で `ne2k_irq` を `irq_register` する形に**この票で移す**
(L-C を待たない。82557 が 3/5/6 を割り当てられた場合に共有するため)。

**共有の約束**: 2 つ目以降の登録は、**既存の全登録者と新規の両方が `IRQ_F_SHARED`** のときだけ受ける。
ハンドラは自分の装置のステータスを読み、要因があれば**落としてから** 1 を返す。1 を返す前に要因を落とすのは、
PC-98 の 8259 (エッジ) で次のエッジを失わないため (TASK_FDC_REALHW §2 の型)。

**ディスパッチ (`irq_dispatch(irq)`、割り込み文脈)**:
1. 表の全登録者を順に呼び、`handled |= fn()` を**集約する** (最初の 1 で打ち切らない — A と B が同時に要因を
   持つ場合に B を取りこぼすと共有線が上がったまま止まる)。
2. `handled` なら**同じ走査をもう 1 回だけ**行う (一巡の途中で別の装置に要因が積まれた場合の回収。予算は 2 巡)。
   2 巡目で誰も受けなければ終了。予算を超えて要因が残る場合は次の割り込み (レベル) か、装置側のポーリング
   (エッジ) に委ねる — ここで無限に回らない。
3. **EOI はここで 1 回だけ** (スレーブなら slave → master の順、`pic_eoi(irq)` の既存実装)。
   誰も受けなかった (`handled == 0`) ときだけ `isr_unexpected_irq(irq)` に**委譲**し、EOI もそちらに任せる
   (既存: 初回診断 + EOI、IRQ15 のスプリアスはスレーブに EOI を送らない特例を保つ)。**二重 EOI 無し**。
4. ディスパッチ中は IF=0 のまま (ネスト無し)。ハンドラは `sti` しない (契約)。

**V86 中**: 動的登録の IRQ は**ホスト所有**として扱い、V86 中でも callback を呼ぶ (反射しない)。共通スタブは
`RESTORE_KSEG` / `cld` / `IRETD_USER` を既存スタブと同じに持つ (V86 のフレームで正しく戻る)。`IRQ_F_V86_REFLECT` は
**予約しない** (反射にはフレームが要り、登録 API の外)。

**寿命と排他 (単一 CPU)**: 表の更新は `irq_save` で囲む。`{fn, arg, flags}` は**一括で有効化** (fn を最後に書く)。
ISR の中からの登録・解除・自己解除は禁止 (契約、debug ビルドで検査)。解除の順序は **装置の要因を止める →
`irq_unregister` → 状態を解放**。`irq_unregister` が戻った後に callback が走らないことを保証する (IF=0 で表から
外し、単一 CPU なので走行中の callback は無い)。PIC のマスクは**登録数で持つ**: 最初の登録で `irq_enable`、
最後の解除で `irq_disable`。共有者が勝手に `irq_disable` を呼ばない (契約)。

### 1-2. 8237 DMA の共通部 — `drivers/dma8237.c` / `dma8237.h`

```c
#define DMA_DIR_TO_MEM   0   /* 装置 → メモリ (読み) */
#define DMA_DIR_FROM_MEM 1
#define DMA_MODE_SINGLE  0   /* 1 回 (FDC。auto-init 無し) */
#define DMA_MODE_CYCLIC  1   /* auto-init (PCM のリング) */
int  dma_chan_setup(unsigned int ch, u32 phys, u32 bytes, int dir, int mode);  /* 0 / 負。**設定後はマスクしたまま返す** */
void dma_chan_unmask(unsigned int ch);
void dma_chan_mask(unsigned int ch);
int  dma_chan_remaining(unsigned int ch, u32 *bytes_left);   /* 0 / 負 (安定読み不能) */
int  dma_above_1mb_state(void);   /* DMA_A20_VERIFIED / DMA_A20_WRITTEN_UNREADABLE / DMA_A20_BLOCKED */
```

- **検査 (負を返して転送しない)**: `bytes` は 1〜65536 (`bytes-1` を書く。0 は禁止 — 64KB 転送に化ける)、
  `phys + bytes - 1` が `phys` と同じ 64KB バンク、`ch` は 0〜3、`phys >= 16MB` は**機種によらず拒否**
  (拡張バンクはこの票で扱わない)、`phys >= 1MB` は `dma_above_1mb_state()` が `BLOCKED` なら拒否。
- **再設定の責任**: 呼び手は再設定の前に `dma_chan_mask` を呼ぶ (設定関数は「マスク中」を前提。マスクされて
  いなければ負)。設定は `irq_save` で囲む — フリップフロップ (`0019h`) はチャネル間で共有され、途中で別チャネルの
  ISR が触ると上位/下位が取り違わる。
- **`dma_chan_remaining` の安定読み**: `irq_save` の中でフリップフロップをリセットし下位→上位を読む、を**2 回**行い、
  2 回の値が一致 (または差が転送単位の範囲内で単調減少) したときだけ返す。3 回で揃わなければ負。
  返すのは「残りバイト数」(= count + 1)。TC 後 (SINGLE) は 0、CYCLIC で再ロード直後は `bytes`。
- **0439h**: `dma8237_init()` (fdc_init より前、`sysclk_detect` の次) で RMW し、結果を 3 値で持つ:
  読み戻しで bit2 が落ちた = `VERIFIED` / 読み戻しが `FFh` などで確認できない = `WRITTEN_UNREADABLE` (NP21/W はこれ。
  **FDC はこの状態で動き続ける** = W2 を壊さない) / bit2 が落ちない = `BLOCKED` (1MB 超の転送を拒否)。
- 純粋関数: `dma_split_addr(phys) → {addr16, bank8}`、`dma_crosses_64k(phys, bytes)`、`dma_count_to_bytes`、
  ポート表の引き。ホスト試験。
- FDC: `dma_setup()` はこの API の薄い呼び出しに (SINGLE、ch2)。**負が返ったら転送せず `fdc_report_fail` の
  phase に "dma" を足して出す** (黙って無視しない)。停止経路 (`fdc_abort_transfer`) の `DMA_MASK_CH2` 直書きも
  `dma_chan_mask(2)` に。

### 1-3. DMA プール — `kernel/dma_pool.c` (pgalloc ではなく固定帯)

**理由 (往復 1 の B8/B9)**: 割り込みは**そのときの CR3 (アプリの PD)** で走る。pgalloc が返すページはアプリ帯に
あり得て、アプリ PD からは非 present か別物理に写像される。全 PD で見えるのは **カーネル帯・SHM・VRAM** (PDE
ごと共有、`kernel/paging.h` 146 行)。したがって DMA バッファは**全 PD で共有される帯の中の固定プール**から出す。

```c
void *dma_pool_alloc(u32 bytes, u32 align, u32 *phys_out);  /* 64KB を跨がない・align 整列・供給元は MEM_DMA_POOL */
void  dma_pool_free(void *virt);
```

- **置き場**: SHM 帯 (224KB、`shm_alloc` の利用者は 2026-09-23 時点で 0 件) から **64KB** を切る:
  `SHM_BLOCK_COUNT` 14 → 10 (160KB)、`MEM_DMA_POOL_SIZE` = 64KB を SHM の直後に (**v3 §2 の数字は決裁 D1 の続き
  として PM が提案、ユーザー決裁**)。プールは 64KB 整列なので**中のどの区間も 64KB を跨がない**。
  写像は supervisor / RW / キャッシュ有効 (PC-98 の 8237 と PCI はスヌープで整合。実機 R7 で確認)。
- **割り当て**: 4KB 単位のビットマップ (16 ビット)、最初適合。`bytes` は 1〜64KB、`align` は 2 の冪で ≤ 64KB、
  0 は 4KB。失敗は NULL + `*phys_out = 0`。**動的確保しない**ので割り込み文脈からも呼べる (`irq_save` で囲む)。
- **解放の契約**: `dma_pool_free` は**装置がそのメモリへの DMA を止めたことを呼び手が確認してから**呼ぶ
  (82557 なら RU/CU の停止と `SCB` の完了待ち、PCM なら `dma_chan_mask` と `remaining` の停止確認)。
  停止を確認できない失敗経路では**解放しない** (プールに「使用中のまま」印を残し、`dma_pool_leaked` を数える)。
- 用途と大きさ: 82557 の CB (数 KB) + RFD 8 本 × 1.5KB ≒ 16KB、PCM リング 16KB、余裕 32KB。
  FDC の 1KB は**静的配列のまま** (pgalloc / SHM より前、起動最初期に要る)。
- 純粋関数: ビットマップの最初適合と整列の判定をホスト試験に。

### 1-4. PCI デバイスの結線表 — `drivers/pci_bind.c`

```c
struct pci_driver {
    u32 size;                                 /* sizeof(struct pci_driver)。将来の外部モジュールが版を名乗る口 */
    u16 vendor, device;                       /* 0xFFFF = 任意 */
    u8  class, subclass;                      /* 0xFF = 任意 */
    const char *name;
    int (*probe)(const struct pci_dev *dev);  /* 0 = 引き受けた / 負 = 断った (次の候補へ) */
};
int pci_bind_all(const struct pci_driver *const *table, int n);   /* 1 件ずつのポインタ配列 (stride を固定しない) */
```

- **一致規則**: 4 欄の AND。`0xFFFF` / `0xFF` は任意。表順に最初に一致した driver の `probe` を呼び、**負なら次の
  候補へ**進む。同じ driver が複数の装置 (別 BDF) に一致してよい — driver は 2 台目を**安全に拒否するか**自分で
  複数を持つ (「一度使ったら候補から外す」はしない)。`probe` に渡す `struct pci_dev` は列挙表の**写し**で、
  probe から戻った後は無効 (driver が要る値は自分で控える)。
- **BAR の検査は driver の責任**: binder は BAR で除外しない (メモリ BAR が 0 でも I/O BAR が有効な装置がある。
  I/O BAR の生値 `1` は「あるが未割り当て」)。driver は `pci_decode.h` で必要な BAR の種別と番地を確かめ、
  無ければ負。
- **probe の段階 (82557 の例、失敗時は逆順に戻す)**: Command の I/O Space Enable → 装置のリセット (PORT) →
  `dma_pool_alloc` で CB/RFD → `irq_register` → Command の Bus Master Enable → CU/RU 開始 → 装置の割り込み許可。
  Command の更新は**下位 16 ビットだけ**書く (上位の Status は W1C なので、読んだ値を書き戻すと消える) —
  `pci_cfg_write16` を足す。
- 表は静的 (`kernel/kernel.c`、最初は 82557 の 1 行)。`pci_bind_all` は **pgalloc と DMA プールの初期化の後**
  (`pci_init` の直後ではない)。§3 の動的読み込みが来たら、`size` で版を確かめる。

### 1-5. µs 時計 — `sys_time_now(u32 *lo, u32 *hi)`

- **時間源**: `tick_count` (1-0 の修正後は**正確に 10ms**) + PIT ch0 のラッチ読み。式は整数で
  `us = tick × period_us + ((reload − count) × period_us) / reload` (`period_us` = 10000、`reload` = 1-0 が記録した値、
  mode 2 (レートジェネレータ) 前提 — mode 3 では 2 ずつ減るので **mode 2 に限定**し、`struct pit_setup.mode` で検査)。
  乗算は u64 (GNU89 の `unsigned long long`、カーネル内のみ) で行い、桁あふれを避ける。
- **IRQ0 との競合**: `t1 = tick` → ラッチ → `count` → `t2 = tick`。`t1 != t2` ならやり直し (最大 3 回)。
  さらに **IF=0 で呼ばれた場合** (IRQ0 が未処理のまま PIT が再ロードし得る) は PIC の IRR bit0 を読み、pending なら
  `tick + 1` として扱う (1 周期ぶんだけ補正。IF=0 が 2 周期 (20ms) を超える呼び手はこの時計の対象外 — 契約に明記)。
- **KAPI**: 64 ビットの戻り値は `ring3_syscall_dispatch` が EDX を返さないので使えない (往復 1 の B14)。
  `sys_time_now(u32 *lo, u32 *hi)` の**出力引数 2 本**で末尾追記 (版数 +1)。CPL=3 のポインタ検査は既存の作法。
- 単調性: `tick_count` は u32 で 497 日で周回。時計は u64 に組み立てるので 71 分の桁あふれは無い。周回は扱わない
  (497 日連続稼働は対象外。契約に明記)。

## 2. 顧客と順序

| 順 | 顧客 | 使う層 |
|---|---|---|
| 0 | 全部 (実機の時間) | 1-0 (PIT のクロック修正) — **最初に単独で着地** |
| 1 | 82557 (L-B) | 1-1 (共有 IRQ)、1-3 (CB/RFD)、1-4 |
| 2 | CS4231 PCM (§5-5 P1) | 1-2 (#1/#3、CYCLIC、remaining)、1-3 (16KB)、1-1 |
| 3 | LGY-98 | 1-1 (3/5/6 をこの票で移す) |
| 4 | FDC | 1-2 (薄い呼び出しに置き換え、負を報告) |

## 3. 受入

| ID | 見るもの | 手段 |
|---|---|---|
| W0 | 1-0: NP21/W で tick 周期が変わらない (`sys_time_now` 1 秒 = 1,000,000 ± 1,000)。**実機で `tick` 100 回 = 1.000 秒** (ホストの時計と突き合わせ。修正前は 0.8125 秒) | NP21/W + 実機 (シリアル越しに `time` を 2 回) |
| W1 | 純粋関数のホスト試験 (変異つき): 共有ディスパッチ (同時要因の集約、2 巡目の回収、誰も受けない、登録の一括有効化と拒否規則)、`dma_split_addr` / `dma_crosses_64k` / `bytes` の範囲 / 安定読みの判定、プールの最初適合と整列、PCI の一致規則 (任意 / 完全 / 負で次へ / 複数装置)、時計の整数式 (両クロック、周期境界、pending 補正) | `check-par` |
| W2 | FDC が `dma8237` 経由でも **NP21/W の 2HD 起動と md5 一致**、読み失敗のタイムアウト → `fdc_abort_transfer` → 再試行の経路。**実機の FD 起動** (0439h は実機でしか効かない) | NP21/W + 実機 |
| W3 | 共通スタブに `irq_register` した偽装置: NP21/W の `/api/pic` で IRR/ISR を見ながら、master (3) と slave (9) の両方、連続、共有 (2 登録の集約)、CPL=3 のアプリ実行中と V86 中、要因を落とさない偽装置で 2 巡で止まること、解除後に callback が走らないこと | NP21/W |
| W4 | `sys_time_now` の単調性 (1 万回連続読みで逆行 0)、両クロック (NP21/W 1.9968 / 実機 2.4576)、周期境界の高頻度読み、CPL=3 から 64 ビットが揃って返る | NP21/W + 実機 |
| W5 | カーネル増分 (`__bss_end` の差) ≤ 5KB。内訳を票に記録 (IRQ 表 768B + コード) | `docs/02_memory.md` |
| W6 | **土台完了**の受入はここまで。82557 での連続 TX/RX・共有・再開は **L-B の受入** (別) | — |

## 4. しないこと

- BAR の自前割り当て (BIOS 未割り当ての装置)。
- 既存の固定 IRQ (0/1/4/7/11/12) の移行。
- 境界モード `0029h` の変更、拡張バンク (16MB 超)。
- 1kHz tick (§5-5 の別票)。
- 外部モジュールの ABI (§3)。`struct pci_driver.size` はそのための口だけ。

## 5. ユーザー決裁が要る点

1. **SHM 224KB → 160KB、DMA プール 64KB** (メモリマップの数字。決裁 D1 の続き)。
2. 1-0 (PIT のクロック修正) を**この票より先に単独で着地**させてよいか (実機の時間が 23% ずれている既存バグ)。
