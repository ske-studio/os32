# TASK_HAL_WIRING — 結線の土台 (割り込みの動的登録 / 8237 DMA の共通部 / DMA 可能メモリ / PCI の結線表 / µs 時計)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 (Codex 設計レビュー待ち)**。
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

## 1. 設計

### 1-1. 割り込みの動的登録 — `kernel/irq.c` / `kernel/irq.h`

```c
typedef int (*irq_handler_fn)(unsigned int irq, void *arg);   /* 1 = 自分の割り込みを処理した / 0 = 自分のではない */
int  irq_register(unsigned int irq, irq_handler_fn fn, void *arg, unsigned int flags);  /* 0 / 負 */
int  irq_unregister(unsigned int irq, irq_handler_fn fn, void *arg);
#define IRQ_F_SHARED  0x01   /* 同じ IRQ に他の登録があっても受ける (PCI 用)。無指定なら 2 つ目の登録は拒否 */
```

- 表: `irq_slot[16][IRQ_MAX_SHARE=4]` の静的配列 (`{fn, arg, flags}`)。**動的確保しない** (割り込み文脈で触る)。
- ディスパッチ: `isr_stub.asm` の `IRQ_UNEXP` を **共通スタブ `irq_stub_common %1`** に置き換え、C の
  `irq_dispatch(irq)` を呼ぶ。`irq_dispatch` は登録順に `fn(irq, arg)` を呼び、**誰かが 1 を返すまで**回す。
  誰も受けなければ従来の `isr_unexpected_irq` (初回だけ診断)。EOI は `irq_dispatch` の末尾で `pic_eoi(irq)`
  (スレーブは両方)。
- **既存の固定ハンドラは当面そのまま** (IRQ 0/1/4/7/11 とマウス、NIC の 3/5/6)。理由: それらのスタブは V86 反射
  (`V86_REFLECT`) や `tick_count` の加算など固有の前処理を持つ。動的登録は **いま `IRQ_UNEXP` になっている
  IRQ (3/5/6/8/9/10/14/15) から**始め、既存の装置は後で必要になったときに移す (移すときも登録 API の中で
  `V86_REFLECT` 相当を扱えるよう、`flags` に `IRQ_F_V86_REFLECT` を予約しておく)。
  ただし **NIC の 3/5/6 (`irq_stub_nic_%1` → `ne2k_irq`) は L-C で NIC 境界を関数表にするときにこの層へ移す**
  (82557 と LGY-98 が同じ入口を使うため)。
- 共有時の約束: ハンドラは **自分の装置のステータスを読んで「自分の割り込みか」を答える**。PCI はレベル
  トリガなので、受けた装置が要因を落とすまで再割り込みが続く — ハンドラは 1 を返す前に要因をクリアする。
  PC-98 の 8259 はエッジなので、要因を落とし損ねると次の割り込みが来ない — これは既存の FDC と同じ罠
  (TASK_FDC_REALHW §2)。
- IRQ 番号は **PCI の Interrupt Line レジスタの値** (BIOS が PIRQ→8259 の写像を済ませた後の 8259 入力番号)。
  `io_pci.md` は写像先のレジスタを書いていないので、Interrupt Line が 0 / 0xFF のときは「未割り当て」として
  ポーリングに落とす (82557 はポーリングでも動く)。

### 1-2. 8237 DMA の共通部 — `drivers/dma8237.c` / `dma8237.h`

```c
int  dma_chan_setup(unsigned int ch, u32 phys, u16 bytes, int dir);  /* dir: DMA_TO_MEM / DMA_FROM_MEM。0 / 負 */
void dma_chan_mask(unsigned int ch);
void dma_chan_unmask(unsigned int ch);
u16  dma_chan_remaining(unsigned int ch);        /* 残りカウント (PCM の再生位置に使う) */
int  dma_enable_above_1mb(void);                 /* 0439h bit2 を RMW で落とす (冪等)。読み戻しを返す */
```

- 表: チャネルごとのポート (アドレス / カウント / バンク / 拡張バンク) を定数表に。`fdc.h` の `DMA_CH2_*` を
  ここへ移し、`fdc.c` の `dma_setup()` はこの API の薄い呼び出しにする (**挙動は変えない** — 実機で通った
  経路を守る)。
- 検査 (呼び手のミスを黙って通さない): `phys` + `bytes` が 64KB 境界を跨げば **負を返して転送しない**
  ([HW2]。境界モード `0029h` は触らない — 起動時の 64KB のまま)。`phys >= 16MB` で拡張バンクの無い機種なら負。
  `phys >= 1MB` で `dma_enable_above_1mb()` が未実行なら負 (起動時に `fdc_init` が呼ぶのを `dma8237_init()` に
  移す)。
- 純粋関数: `dma_split_addr(phys) → {addr16, bank8, ext8}`、`dma_crosses_64k(phys, bytes)`、ポート表の引き。
  ホスト試験の対象。
- 排他: チャネルの設定は `irq_save` で囲む (FDC の ISR と PCM の ISR が別チャネルを同時に触っても、
  8237 のフリップフロップ (`0019h`) は**共有**なので、設定の途中で割り込まれると上位/下位が取り違わる)。

### 1-3. DMA 可能メモリ — `kernel/dma_alloc.c`

```c
void *dma_alloc(u32 bytes, u32 align, u32 limit_phys, u32 *phys_out);  /* 物理連続、align 整列、64KB を跨がない、limit_phys 未満 */
void  dma_free(void *virt, u32 bytes);
#define DMA_LIMIT_16MB  0x01000000UL   /* 8237 (拡張バンク無し) */
#define DMA_LIMIT_4GB   0xFFFFFFFFUL   /* PCI バスマスタ */
```

- 実装: `pgalloc_alloc_n_pfn(n, first, end, &pfn)` でページ単位に取り、**64KB 境界を跨ぐ組は捨てて取り直す**
  (最大 8 回)。物理 = 仮想はカーネル帯の恒等写像に限る — `pgalloc` が返す PFN がカーネルの写像に入っているか
  (`paging` の問い合わせ) を確かめ、入っていなければ張る。返す `phys_out` は装置に渡す番地。
- 用途と大きさ: 82557 の CB/RFD リング (数 KB、4GB 限度)、PCM のリング (16KB、16MB 限度、64KB 内)。
  FDC の 1KB は**静的配列のまま** (起動の最初期、pgalloc より前に要るため)。
- 純粋関数: `dma_pages_ok(pfn_first, n, align, limit)` (跨ぎ・整列・上限の判定) をホスト試験に。

### 1-4. PCI デバイスの結線表 — `drivers/pci_bind.c`

```c
struct pci_driver {
    u16 vendor, device;                       /* 0xFFFF = 任意 */
    u8  class, subclass;                      /* 0xFF = 任意 */
    const char *name;
    int (*probe)(const struct pci_dev *dev);  /* 0 = 引き受けた / 負 = 断った */
};
int pci_bind_all(const struct pci_driver *table, int n);   /* 起動時に 1 回。列挙表の各装置に最初に一致した driver の probe を呼ぶ */
```

- 表は **静的** (`kernel/kernel.c` の配列。最初は 82557 の 1 行)。§3 の動的読み込みが来たら、外部モジュールが
  同じ `struct pci_driver` を差し出す形にする — **構造体の並びと意味をここで固定**する (KAPI と同じく末尾追記)。
- `probe` に渡すのは列挙表の写し (`struct pci_dev`、40 バイト)。BAR は生値なので、`pci_decode.h` の復号を使う。
  **BIOS が BAR を割り当てていない (0) 装置は probe に渡さない** (自分で割り当てるのは別票)。
- Command レジスタの I/O Enable / Bus Master は **probe の中で driver が立てる** (`pci_cfg_write32` は既にある)。

### 1-5. µs 時計 — `sys_time_us()`

```c
u64 sys_time_us(void);    /* 単調。tick_count × 10000 + PIT ch0 のラッチ読みから求めた端数 */
```

- PIT ch0 は OS32 が 100Hz (`PIT_HZ`) の方形波/レートで回している。ラッチ (`0077h` にラッチ命令、`0071h` で
  2 バイト読み) で現在カウントを取り、`(初期値 − カウント) / (クロック / 1e6)` を µs に。クロックは
  `serial_detect_clock()` と同じ `0000:0501h` bit7 (2.4576 / 1.9968MHz)。
- **tick の境界での取り違え** (ラッチの直後に IRQ0 が来て `tick_count` が進む) は、読み取りを
  「tick を読む → ラッチ → tick を読み直す → 違えばやり直す」で塞ぐ (最大 2 回)。
- 1kHz tick への引き上げ (§5-5) は**別票** — この票は読み出しだけ。KAPI に出す (末尾追記、版数 +1) のは
  実機の性能計測にすぐ効くので同じ票で。

## 2. 顧客と順序

| 順 | 顧客 | 使う層 |
|---|---|---|
| 1 | 82557 (L-B) | 1-1 (共有 IRQ)、1-3 (CB/RFD、4GB)、1-4 |
| 2 | CS4231 PCM (§5-5 P1) | 1-2 (#1/#3)、1-3 (16KB、16MB、64KB 内)、1-1 |
| 3 | LGY-98 (L-C で NIC 境界を関数表に) | 1-1 (3/5/6 を移す) |
| 4 | FDC | 1-2 (薄い呼び出しに置き換え。挙動不変) |

## 3. 受入

| ID | 見るもの | 手段 |
|---|---|---|
| W1 | 純粋関数のホスト試験: 共有ディスパッチの順序と「誰も受けない」、`dma_split_addr` / `dma_crosses_64k`、`dma_pages_ok`、PCI の一致規則 (任意 / 完全)、µs の換算と tick 境界の取り違え | 変異つき、`check-par` |
| W2 | FDC が `dma8237` 経由でも **NP21/W の 2HD 起動と md5 一致** (TASK_FD144 F9 相当) | NP21/W |
| W3 | `IRQ_UNEXP` だった IRQ に `irq_register` した偽の装置で 1 回割り込みを起こし、ディスパッチ → EOI → 再割り込みが来ること (NP21/W の `/api/pic` で IRR/ISR を見る) | NP21/W |
| W4 | `sys_time_us()` が 1 秒で 1,000,000 ± 100 (tick と PIT の整合)、単調 | NP21/W + 実機 |
| W5 | カーネル増分 ≤ 5KB (`docs/02_memory.md`) | ビルド |
| W6 | 実機: 82557 の Interrupt Line で `irq_register` → CU 完了 1 回の割り込みが届く (L-B の R6) | 実機 |

## 4. しないこと

- BAR の自前割り当て (BIOS 未割り当ての装置) — 実機で 0 だったときに別票。
- 既存の固定 IRQ (0/1/4/7/11/マウス) の移行 — 必要になるまで。
- 境界モード `0029h` の変更、拡張バンク (16MB 超) の使用 — 顧客が現れるまで。
- 1kHz tick — §5-5 の別票。
