# TASK_HAL_WIRING — 結線の土台 (割り込みの動的登録 / 8237 DMA の共通部 / DMA プール / PCI の結線表 / µs 時計)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 v4 (Codex 往復 3 の B1〜B7 + 非 blocker を反映。規定の 3 往復を使い切ったので、追加 1 往復か着地かはユーザー判断)**。
> ユーザー指示 2026-09-23: 「結線の土台の票の設計を先に起こす」。
> 往復記録: v1 → Codex 往復 1 (B1〜B14、Request changes) → v2 → Codex 往復 2 (R1〜R9、Request changes) → v3 → Codex 往復 3 (B1〜B7、Request changes。R2/R3/R4/R6 は閉、R1/R5/R7/R8/R9 は部分) → v4。

正典の関係: [`PLAN.md`](PLAN.md) §3-1 (HAL の棚卸し)、§3 (ドライバの動的読み込み — この票の「取り決め」を後で外部モジュールに開く)、
[`../realhw/TASK_LAN_82557.md`](../realhw/TASK_LAN_82557.md) (L-B 82557 が最初の顧客)、§5-5 (PCM リングとタイマが 2 番目の顧客)。

## 0. なぜ要るか

PCI の装置は **BIOS が IRQ を割り当て、複数の装置が 1 本の IRQ を共有し得る**。いまの OS32 は `kernel/isr_stub.asm` が
装置ごとに固定の C ハンドラを `extern` で結んでおり (`timer_handler` / `kbd_irq_handler` / `serial_irq_handler` /
`fdc_irq_handler` / `mouse_irq_handler` / `ne2k_irq` (`irq_stub_nic_3/5/6`)、それ以外は `IRQ_UNEXP` で EOI だけ)、
**起動時に読んだ IRQ 番号でハンドラを結ぶ口が無い**。DMA も `dma_setup()` が `drivers/fdc.c` の中で ch2 決め打ち、
DMA に使えるメモリは静的配列 + 整列で取っている。82557 (バスマスタ、IRQ は BIOS 次第) と CS4231 (8237 の #1/#3) を
載せるには、この 4 つを装置から切り離した層にする必要がある。

**v3 で変えたこと (Codex 往復 2)**: 共有 IRQ の**処理継続**は「タイマ tick の回収走査 + ストーム時のマスク」で契約化 (R1)、
EOI は `irq_finish()` の 1 経路で、IRQ15 のスプリアス検査を handled から独立 (R2)、クロック判定は **PG=0 の起動専用経路**で
BIOS ワークを読んで保存 (R3)、DMA プールは **SHM ではなく SQLite 帯の予約域の固定番地** (SHM・GUI オフセット・SDK・
アプリは無変更、R4/R5)、プールは span 表で `free` を識別 (R6)、`remaining` は **TC の保持 + 一致規則の明文化** (R7)、
時計は **IRR を挟み読みするスナップショット手順** (R8)、probe の失敗は「戻して次へ」と「隔離して打ち切り」を区別 (R9)。

## 1. 設計

### 1-0. 前提の修正: PIT の分周をクロック判定に合わせる (既存バグ)

`kernel/idt.c` の `pit_init()` は `PIT_CLOCK` = 1,996,800Hz 決め打ちで分周する。**2.4576MHz 系 (実機 Ra266 は
`0000:0501h` bit7 = 0)** では 100Hz を頼んでも **tick = 8.125ms (123Hz)** になっており、`tick_count` を使う待ち・番犬・
校正すべてが 23% 速い。

- **`sysclk_detect()` (新設、`kernel/sysclk.c`)**: `paging_init()` より**前** (PG=0、物理番地がそのまま見える) に
  `kernel.c` から 1 回だけ呼び、`0000:0501h` bit7 を読んで `g_sysclk_hz` (1996800 / 2457600) と `g_sysclk_8mhz` に保存する。
  以後は**保存値だけを読む**。既存の `serial_detect_clock()` は CR3 が master と一致しないと何もしない (PG=0 では
  CR3 が未設定なので現状は**検出をスキップして既定値のまま**) — これを保存値の参照に置き換え、CR3 検査を消す。
  シリアルの `serial_init` (KAPI 経由、CPL=3) も同じ保存値を見る。
- **`pit_init(hz)`**: `reload = g_sysclk_hz / hz`。**この票では hz = 100 に限定** (両クロックで reload = 19968 / 24576、
  整数、周期はちょうど 10ms)。100 以外を頼まれたら負を返して既定 100 にする (1kHz は §5-5 の別票で、
  そのときは実周期 = `reload × 1e6 / g_sysclk_hz` を µs で持つ汎用式に広げる)。`struct pit_setup { u32 hz, reload,
  period_us, clk_hz; u8 mode; }` を記録する (1-5 が読む。mode は `PIT_MODE_TIMER0` = 0x34 = mode 2)。
- 順序: `sysclk_detect → pit_init → irq_enable(0) / sti → cpu_calibrate` (kernel.c 184〜204 の並びに `sysclk_detect` を
  足すだけ。校正は PIT の後という現状を保つ)。
- 効果: NP21/W (1.9968MHz 設定) では reload = 19968 のまま**不変**。実機では tick が 10ms になり、
  `cpu_delay_us()` の実時間・FDC のタイムアウト (tick 単位)・シリアルの TX 予算 (tick 単位) が**全部変わる**ので、
  W0 に回帰を入れる。この票の**最初の着地単位** (決裁 2)。
- **進捗 (2026-09-23、worktree `wt/pit-clock`)**: 実装済み。`kernel/sysclk.{c,h}` / `kernel/pit_math.{c,h}` を新設、
  `pit_init` は `int` を返して `pit_get_setup()` で `struct pit_setup` を公開、`serial_detect_clock()` は廃止して
  シリアルも保存値を見る。ホスト試験 `make check-pit-clock-host` (5 ケース / 変異 5 本すべて RED) と
  kselftest の `test_pit_setup()` を追加、経緯は `docs/POLICY_DEBUG.md` §4-54。
  `make kernel` は警告の増加なしで通る。**W0 (実機での tick 実時間の回帰) は PM の検証待ち**。

### 1-1. 割り込みの動的登録 — `kernel/irq.c` / `kernel/irq.h`

```c
#define IRQ_NONE      0   /* 自分の要因ではない */
#define IRQ_HANDLED   1   /* 自分の要因を見つけて落とし、残件は無い */
#define IRQ_DEFERRED  2   /* 自分の要因だが、予算超過 / busy で残件がある (装置側の割り込みはマスクしてある) */
typedef int (*irq_handler_fn)(unsigned int irq, void *arg);
int  irq_register(unsigned int irq, irq_handler_fn fn, void *arg, unsigned int flags);
int  irq_unregister(unsigned int irq, irq_handler_fn fn, void *arg);
#define IRQ_F_SHARED  0x01
```

**対応 IRQ (契約)**: 動的登録を受けるのは**共通スタブ `irq_stub_common %1` に結ばれた IRQ だけ**。この票で共通スタブに
するのは、いま `IRQ_UNEXP` の 3/5/6/8/9/10/14/15 と、`irq_stub_nic_3/5/6` (LGY-98) の 3/5/6。
それ以外 (0 タイマ / 1 KBD / 2 VSYNC (V86) / 4 シリアル / 7 スプリアス / 11 FDC / 12 サウンド (V86) / 13 マウス) は
固定スタブのままで、`irq_register` は **`-ENOTSUP` (負) で拒否**する。範囲外 (≥16) も拒否。呼び手 (82557) は
拒否されたら装置側の割り込みを止めてポーリングへ落とす。LGY-98 は `lgy98_init()` で `ne2k_irq` の**アダプタ**を
`irq_register` する形に**この票で移す** (下記)。

**共有の約束**: 2 つ目以降の登録は、**既存の全登録者と新規の両方が `IRQ_F_SHARED`** のときだけ受ける。1 IRQ あたり
最大 4 登録。ハンドラは自分の装置のステータスを読み、要因があれば**落としてから** `IRQ_HANDLED` を返す
(PC-98 の 8259 は `ICW1_INIT` = 0x11 の**エッジ**設定。要因を落とさないと次のエッジが来ない)。
ハンドラは**有界**でなければならない: 装置待ちのスピンは禁止、処理は自分の予算 (フレーム数など) で打ち切って
`IRQ_DEFERRED` を返す。debug ビルドは `sys_time_now` でハンドラ時間を測り、1ms 超を 1 回だけ報告する。

**ディスパッチ (`irq_dispatch(irq)`、割り込み文脈、IF=0、ネスト無し)**:
1. 表の全登録者を順に呼び、`any |= rc` を**2 巡ぶん累積**する (`handled_any` は 2 巡全体の OR。2 巡目の 0 で
   上書きしない)。最初の `IRQ_HANDLED` で打ち切らない (A と B が同時に要因を持つ場合に B を取りこぼすと
   共有線が上がったまま次のエッジが来ない)。
2. 1 巡目で誰かが受けたら**同じ走査をもう 1 回だけ**行う (一巡の途中で別の装置に要因が積まれた場合の回収)。
3. **処理継続の契約 (R1)**: 2 巡目でも誰かが `IRQ_HANDLED` / `IRQ_DEFERRED` を返した、または誰かが `IRQ_DEFERRED` を
   返したときは、`irq_poll_pending |= 1 << irq` を立てて戻る。**`timer_handler` (IRQ0、10ms) が `irq_poll_pending`
   の立っている IRQ について同じ走査 (1 巡) を実行**し、全員が `IRQ_NONE` を返すまで tick ごとに続ける
   (`ne2k_timer_tick` の型を一般化したもの。`irq_finish` は呼ばない — EOI はハード IRQ の経路だけ)。
   - **pending の寿命 (往復 3 B1)**: pending は IRQ 単位だが、**登録者が 1 人でも残っている間は消さない**。
     `irq_unregister` は自分を表から外すだけで、表が空になったときだけ pending を落とす。
   - **IRQ0 の予算 (往復 3 B2)**: 回収走査は **1 tick あたり合計 2ms** (`pit` の count で測る。`IRQ_POLL_BUDGET_US`
     = 2000) を上限とし、超えたら残りの IRQ は次の tick へ持ち越す。開始 IRQ は tick ごとに**ラウンドロビン**
     (前回の続きから) で公平にする。callback は 1 件 1ms 未満の契約なので、最悪でも 2ms + 1 件分 = 3ms で IRQ0 が
     終わり、1-5 の「IF=0 < 10ms」の前提を自分で破らない。既存の `ne2k_timer_tick` は**この走査より前**に呼び、
     その所要時間も同じ予算から引く (超えていれば走査はその tick を飛ばす)。
   - **NE2000 の待ちと reset (往復 3 B2)**: `wait_rdc` (最大 4ms) と `hw_reset` (最大 20ms) は**既存の ISR 文脈の
     挙動**で、この票では変えない。アダプタは `service()` だけを呼び、reset に進む経路 (`NE2K_STATE_FAILED` からの
     再初期化) は既存どおり `ne2k_timer_tick` にだけ残す。契約違反 (1ms 超) として `irq_handler_slow` に記録し、
     foreground への移設は L-C の票で扱う (残件に明記)。
   - **無進捗の検出**: `IRQ_POLL_MAX_TICKS` (= 100) は「**100 tick 連続で `IRQ_DEFERRED` だけが返り、1 度も
     `IRQ_HANDLED` が無い**」を無進捗とみなす基準 (常時負荷で `HANDLED` が続くのは正常で、数えない)。
     無進捗なら `irq_poll_starved` を数えて 1 回報告し、driver は `irq_stall_count(irq)` を見て自分の watchdog で
     装置をリセットする (この層は装置を触らない)。
   走査中に本物の IRQ が来ても、単一 CPU・IF=0 なので走査は直列化される。
   反例 (A の新しい要因が B の保持中に来て線が下がらない) は、この tick 回収で 10ms 以内に拾う。
4. **ストーム**: 1 tick の間に同じ IRQ のディスパッチが `IRQ_STORM_LIMIT` (= 200) 回を超えて `handled_any == 0`
   (誰も受けない) なら、PIC でその IRQ をマスクして `irq_storm_masked |= 1 << irq`、1 回だけ報告する。
   復帰は次の `irq_register` / `irq_unregister` の**登録数の再計算時**に行う (driver が装置を直してから登録し直す)。
   レベルトリガは**扱わない** (現行 PIC はエッジ。「しないこと」に明記)。
5. **終了経路は 1 つ、`irq_finish(irq, handled_any)` (R2)**:
   - `irq == 15` なら、**handled に関係なく**スレーブの ISR (OCW3 = 0x0B) を読み、bit7 が立っていなければスプリアス
     として**マスタにだけ EOI** (`isr_unexpected_irq` の現行と同じ)。立っていれば `pic_eoi(15)`。
     IRQ7 のスプリアスは固定スタブのままなので対象外。読みのあと OCW3 = 0x0A (IRR) に戻す。
   - それ以外は `pic_eoi(irq)` (スレーブ → マスタ)。
   - `handled_any == 0` なら **`isr_unexpected_report(irq)`** を呼ぶ (診断だけ。現行 `isr_unexpected_irq` から
     EOI を抜いた関数。`IRQ_UNEXP` スタブは共通スタブに置き換えるので、EOI を送る経路は `irq_finish` だけになる)。
   - ここが `pic_eoi` を呼ぶ唯一の動的経路 (固定スタブは各自の EOI を保つ)。
6. 共通スタブは `RESTORE_KSEG` / `cld` / `IRETD_USER` を既存スタブと同じに持つ。**IRQ0 の固定スタブにも C 呼び出しの
   前に `cld` を足す** (往復 3 B3: 割り込み入口は DF を自動で消さない。V86 ゲストが `STD` の直後に IRQ0 を受けると、
   tick 回収から呼ばれた callback の `kstrcmp` (`lodsb`) が逆向きに走る)。他の固定スタブ (1/4/11/13) は C 呼び出し
   の前に `cld` があるかを実装時に確かめ、無ければ同じく足す。**V86 中**は動的登録の IRQ を
   ホスト所有として扱い、反射しない。

**LGY-98 のアダプタ (R1)**: `ne2k_irq_shared(irq, arg)`: (a) `nic.busy` なら `irq_pending = 1` を立てて
`IRQ_DEFERRED` (leave() が処理し、それまでの tick 回収は `busy` を見て何もしない)。(b) `ISR & IMR_MASK` が 0 で
`rx_backlog` も 0 なら `IRQ_NONE`。(c) それ以外は現行 `ne2k_irq()` の本体 (service + recheck) を走らせ、戻りに
`rx_backlog` か **IMR に最後に書いた値 (`nic.imr_written`、新設。`imr_mask` は復帰時の設定値なので判定に使わない)**
が 0 なら `IRQ_DEFERRED`、無ければ `IRQ_HANDLED`。`lgy98_init()` の `irq_is_free` (PIC のマスクで空きを見る) は
**廃止**し、`irq_register` の戻りで判定する (共有登録に合わせる)。`ne2k_timer_tick` は残す (tick 回収と役割が重なるが、
OVW 復旧と送信タイムアウトはそちらだけが持つ)。

**寿命と排他 (単一 CPU)**: 表の更新は `irq_save` で囲む。`{fn, arg, flags}` は**一括で有効化** (fn を最後に書く)。
ISR の中からの登録・解除・自己解除は禁止 (契約、debug ビルドで検査)。解除の順序は **装置の要因を止める →
`irq_unregister` → 状態を解放**。`irq_unregister` が戻った後に callback が走らないことを保証する (IF=0 で表から
外し、`irq_poll_pending` のその IRQ も落とす。単一 CPU なので走行中の callback は無い)。PIC のマスクは**登録数で持つ**:
最初の登録で `irq_enable`、最後の解除で `irq_disable`。共有者が勝手に `irq_disable` を呼ばない (契約)。
遅延処理 (tick 回収) は「解除で消える」以外を持たない (driver 側の遅延はこの層の外)。

### 1-2. 8237 DMA の共通部 — `drivers/dma8237.c` / `dma8237.h`

```c
#define DMA_DIR_TO_MEM   0   /* 装置 → メモリ (8237 の write transfer) */
#define DMA_DIR_FROM_MEM 1   /* メモリ → 装置 (read transfer) */
#define DMA_MODE_SINGLE  0   /* auto-init 無し (FDC)。8237 の転送モードは single transfer、番地は増分 */
#define DMA_MODE_CYCLIC  1   /* auto-init 有り (PCM のリング)。転送モードは同じく single transfer、番地は増分 */
int  dma_chan_setup(unsigned int ch, u32 phys, u32 bytes, int dir, int mode);  /* 0 / 負。**設定後はマスクしたまま返す** */
void dma_chan_unmask(unsigned int ch);
void dma_chan_mask(unsigned int ch);
int  dma_chan_remaining(unsigned int ch, u32 *bytes_left, int *tc_seen);   /* 0 / 負 (安定読み不能) */
void dma_chan_ack_tc(unsigned int ch);          /* 保持している TC を消す */
int  dma_above_1mb_state(void);   /* DMA_A20_VERIFIED / DMA_A20_UNREADABLE / DMA_A20_BLOCKED (診断) */
```

- **検査 (負を返して転送しない)**: `bytes` は 1〜65536 (`bytes-1` を書く。0 は禁止)、`phys + bytes - 1` が `phys` と
  同じ 64KB バンク、`ch` は 0〜3、`dir` / `mode` は上の値以外を拒否、`phys >= 16MB` は**機種によらず拒否**
  (拡張バンクはこの票で扱わない)。demand / block モード・番地減分は口を持たない。
- **0439h (診断であって、転送の可否に使わない)**: `dma8237_init()` (fdc_init より前、`sysclk_detect` の次) で
  書く前の値 `pre` と RMW 後の読み戻し `post` を保存し、`post & 0x04` が 0 → `VERIFIED`、`post == 0xFF` →
  `UNREADABLE`、それ以外 → `BLOCKED`。**プールは構造上 1MB 超 (0x2E8000) にあり、FDC の静的バッファもカーネル
  (0x100000〜) にある**ので、BLOCKED の機種では OS32 の FDC 自体が動かない (W2 の実機 FD 起動がそれを検出する)。
  よって `dma_chan_setup` は 1MB 超を状態で拒否しない (往復 1 の B7 から変更。理由は上)。3 値は起動行と
  `pcidump` 相当の診断に出す。`VERIFIED` は「ビットが落ちた」であって動作実証ではない (実証は W2)。
- **再設定の責任**: 呼び手は再設定の前に `dma_chan_mask` を呼ぶ (設定関数はマスク中を前提。マスクされて
  いなければ負)。設定は `irq_save` で囲む — フリップフロップ (`0019h`) はチャネル間で共有され、途中で別チャネルの
  ISR が触ると上位 / 下位が取り違わる。
- **TC の保持 (R7、往復 3 B4/B5)**: 8237 のステータス (`0011h`) は**読むと全チャネルの TC が消える**。共通部だけが
  これを読み、読んだ値の TC ビットを**2 つの状態**に写す: `done[ch]` (終了済み。**次の `dma_chan_setup` まで
  保持**、消費されない) と `tc_event[ch]` (通知。`dma_chan_ack_tc` で消す。CYCLIC では周回ごとに立つ)。
  `dma_chan_setup` は自分のチャネルの両方を消す (その前にステータスを 1 回読んで他チャネルの分を保存する)。
- **`dma_chan_remaining` の手順 (R7、往復 3 B4/B5)**: `irq_save` の中で
  1. ステータス読み → `done` / `tc_event` 更新。**SINGLE で `done` なら `bytes_left = 0`、`tc_seen = 1` で即返す**
     (count の採用判定より**先**。TC 後の `FFFFh` は設定長超で不採用になるので、count は見ない)。
  2. FF クリア → 下位 → 上位 でカウント `c1`。
  3. **ステータス再読み** → 更新。SINGLE で `done` なら 0 を返す (`c1` の読みの直後に終わった場合。`irq_save` は
     DMA を止めないので、count とステータスは**一体**として扱う)。
  4. 同じ手順で `c2`、そのあと**もう 1 度ステータス** → SINGLE で `done` なら 0。
  5. **採用条件**: `c1 == c2`、または `c1 > c2` かつ `c1 - c2 <= 64` (2 回の読みの間に進み得る転送量の上限)。
     採用は**後の値 `c2`**。どちらかが設定長 (`bytes - 1`) を超えたら合成が壊れている (再ロードや桁借りを
     またいだ) として不採用。不採用なら 2〜4 をやり直し、**3 組**で揃わなければ `-EAGAIN`。
  6. 返す `bytes_left` = `c2 + 1`。`tc_seen` = `tc_event`。
  CYCLIC では `bytes_left` は**現在の周回の中の位置**でしかない: `tc_event` は「前回の ack 以降に 1 周以上完了」
  しか意味せず、周回数・停止・アンダーランは装置側の割り込み (CS4231 の half/full) で判定する (契約に明記。
  remaining を停止の証拠に使わない)。`done` は CYCLIC では立てない (auto-init は終わらない)。
- 純粋関数: `dma_split_addr(phys) → {addr16, bank8}`、`dma_crosses_64k(phys, bytes)`、`dma_count_to_bytes`、
  安定読みの採用判定 `dma_accept_pair(c1, c2, limit)`、ポート表の引き。ホスト試験。
- FDC: `dma_setup()` は **`dma_chan_mask(2)` → `dma_chan_setup(2, …, SINGLE)` → 成功したときだけ `dma_chan_unmask(2)`**
  の 3 段 (setup だけに置き換えるとマスクしたままコマンドを出してタイムアウトする)。**負が返ったら FDC コマンドを
  発行せず `fdc_report_fail` の phase に "dma" を足して出す**。`fdc_abort_transfer` と `fdc_recover` (fdc.c 480 行) の
  `DMA_MASK_CH2` 直書きも `dma_chan_mask(2)` に。

### 1-3. DMA プール — `kernel/dma_pool.c` (固定番地、SQLite 帯の予約域)

**理由 (往復 1 の B8/B9、往復 2 の R4/R5)**: 割り込みは**そのときの CR3 (アプリの PD)** で走る。全 PD で共有される
のは PDE 0 (0〜4MB) 全体 (`kernel/paging.h` 151 行)。v2 の「SHM 帯から切る」は (a) SHM の基点が `__bss_end` 由来の
4KB 整列で 64KB 整列にならない、(b) `MEM_SHM_SIZE` を減らすと `MEM_SHM_GUI_OFFSET` が動いて SDK の `GUI_SHM_OFFSET`
(C / Rust) と GUI アプリ全部の再ビルドが要る、の 2 点で取り下げる。

- **置き場**: SQLite 帯の「カーネル予約」(現在 0x2DD000〜0x2FAFFF、120KB、NOT PRESENT) の中の**固定番地**
  `MEM_DMA_POOL_BASE` = **0x2E8000**、`MEM_DMA_POOL_SIZE` = 0x10000 (0x2E8000〜0x2F7FFF)。純粋な定数式なので
  `STATIC_ASSERT(MEM_DMA_POOL_BASE + MEM_DMA_POOL_SIZE <= MEM_STACK_GUARD)` で固定できる。上下は予約域のまま
  NP (下: SQLite 代替スタックの後、上: 0x2F8000〜0x2FAFFF の 12KB) がガードになる。
  SQLite の成長は `build/os32.ld` で止める。リンカは C ヘッダを読まないので、`MEM_SQLITE_STACK_SIZE = 0x20000;` と
  `MEM_DMA_POOL_BASE = 0x2E8000;` を**絶対シンボルとして ld に持ち** (`MEM_KSTACK_TOP` と同じ作法、
  `tools/gen_memmap.py --check` が C 側と照合)、
  `ASSERT(__sqlite_end + MEM_SQLITE_STACK_SIZE + 0x1000 <= MEM_DMA_POOL_BASE, "SQLite band overruns DMA pool")`
  を書く (いまの余裕 44KB。SQLite は amalgamation で固定に近い)。
  `MEM_KERNEL_IMAGE_MAX` (カーネル帯の予算) は**触らない** (プールはカーネル帯の外)。SHM・GUI・SDK・アプリは無変更。
- **写像**: `paging_init` が `paging_set_not_present` の範囲からプールを外し、**present / supervisor / RW / キャッシュ
  有効**で張る。アプリ PD は master の PDE 0 を写すので同じ PT を見る。**USER ビットが立たないこと**を W5 で確かめる。
  現行の MM 検査 (`kernel/paging.c` 1149〜、`memmap_seen_at`) は RW を見た時点で `MM_RW` を返し **USER 混入を
  見分けない** (往復 3 B6) ので、`MM_RWU` (present + RW + USER) を足して `MM_RW` と区別し、期待値側 (`memmap_expect_at`)
  は**予約域を NP とする分岐より先に**プールを `MM_RW` と判定する。変異試験: プールの PTE に USER を立てると W5 が落ちる。
- **64KB 境界**: プールは 0x2F0000 を跨ぐので、`dma_pool_alloc` は候補ごとに `dma_crosses_64k` を検査して跨ぐ候補を
  飛ばす。**1 件の span の上限は 32KB** (境界の両側がそれぞれ 32KB)。33〜64KB の要求は空でも必ず失敗する
  (契約に明記)。≤ 32KB が必ず入るのは**空のプール**の話で、断片化後は失敗し得る (呼び手は NULL を扱う)。
- **割り当て (R6)**: 4KB 単位 16 ページのビットマップ + **span 表** `struct { u8 first, npages, state; }` × 16
  (state = FREE / USED / LEAKED)。`dma_pool_alloc(bytes, align, *phys_out)`: `bytes` は 1〜64KB、`align` は 2 の冪で
  ≤ 64KB、0 は 4KB。成功で virt (= phys、恒等) と span の登録、失敗は NULL + `*phys_out = 0`。
  `dma_pool_free(virt)`: **span の先頭と一致するときだけ**解放。途中ポインタ・二重解放・範囲外は負
  (`-EINVAL`) を返して `dma_pool_bad_free` を数える。`dma_pool_mark_leaked(virt)`: span を LEAKED にして
  `dma_pool_leaked` を数える (停止を確認できない失敗経路で driver が明示的に呼ぶ。LEAKED は再利用しない)。
  alloc / free / mark はいずれも `irq_save` の短い排他の中で表だけを触る (探索 + 表更新、動的確保なし) ので
  割り込み文脈から呼べる。`dma_pool_init` 前は全部負 / NULL。
- **解放の契約 (R6/R9、往復 3 B7)**: `dma_pool_free` は**装置がそのメモリへの DMA を止めた証拠を呼び手が持ってから**
  呼ぶ。証拠の定義は装置ごと: 82557 では **CUS/RUS の Idle だけは証拠にしない** (SDM §6.3.2.1: CU Start の直後は
  しばらく Idle が見える。SCB command byte = 0 も「受理」であって完了ではない)。**開始命令 (CU Start / RU Start) を
  1 度でも発行した後は、PORT selective reset (PORT = 0010、CU/RU を止め設定を保つ) を必須**とし、10µs 待って
  CUS = Idle かつ RUS = Idle を読む。これが証拠。reset 後も Idle にならなければもう 1 回 reset、それでも駄目なら
  **隔離** (下記)。開始命令を出す前 (probe の (5) まで) は DMA は起きていないので証拠は要らない。PCM は
  `dma_chan_mask(ch)` + CS4231 の再生許可ビットを落とした後の**装置側**の確認 (remaining の一致は証拠にしない)。
- 用途と大きさ: 82557 の CB (数 KB) + RFD 8 本 × 1.5KB ≒ 16KB、PCM リング 16KB、余裕 32KB。
  FDC の 1KB は**静的配列のまま** (起動最初期に要る)。
- 純粋関数: 最初適合 + 整列 + 64KB 跨ぎの判定、span の登録 / 解放 / 不正解放をホスト試験に。

### 1-4. PCI デバイスの結線表 — `drivers/pci_bind.c`

```c
#define PCI_PROBE_OK          0
#define PCI_PROBE_DECLINE    -1   /* 装置に触っていない、または完全に戻した → 次の候補へ */
#define PCI_PROBE_QUARANTINE -2   /* 装置を未知の状態に残した → その BDF は候補探索を打ち切り、隔離 */
struct pci_driver {
    u32 size;                                 /* sizeof(struct pci_driver)。将来の外部モジュールが版を名乗る口 */
    u16 vendor, device;                       /* 0xFFFF = 任意 */
    u8  class, subclass;                      /* 0xFF = 任意 */
    const char *name;
    int (*probe)(const struct pci_dev *dev);
};
int pci_bind_all(const struct pci_driver *const *table, int n);   /* 1 件ずつのポインタ配列 */
```

- **一致規則**: 4 欄の AND。`0xFFFF` / `0xFF` は任意。表順に最初に一致した driver の `probe` を呼び、**DECLINE なら
  次の候補へ**、**QUARANTINE ならその BDF の探索を打ち切って `pci_quarantined` に記録** (`lspci` が `[quarantined]`
  を出す)。同じ driver が複数の装置 (別 BDF) に一致してよい — driver は 2 台目を DECLINE するか自分で複数を持つ。
  `probe` に渡す `struct pci_dev` は列挙表の**写し**で、probe から戻った後は無効。
- **BAR の検査は driver の責任** (binder は BAR で除外しない。I/O BAR の生値 `1` は「あるが未割り当て」)。
- **probe の段階 (82557 の例) と失敗時の戻し**: (1) Command の I/O Space Enable → (2) PORT selective reset + 10µs →
  (3) `dma_pool_alloc` で CB/RFD → (4) `irq_register` → (5) Command の Bus Master Enable → (6) CU/RU 開始 →
  (7) 装置の割り込み許可。**(5) までの失敗は逆順に戻して DECLINE** (装置は触っていないか reset 直後) — ただし **(2) の reset が Idle を
  確立できなかった場合は例外で QUARANTINE** (状態不明の装置を次の driver に渡さない)。
  **(6) 以降の失敗**は selective reset を打って停止の証拠 (1-3) を取り、取れれば逆順に戻して DECLINE、取れなければ
  **QUARANTINE**:
  Bus Master Enable を落とし (それ自体は書ける)、span を `dma_pool_mark_leaked`、`irq_register` した callback は
  **解除せず「排出モード」に切り替える** (装置のステータスを読んで ack するだけで何も処理しない。callback が
  参照する状態は解放しない)。
- Command の更新は**下位 16 ビットだけ**書く (`pci_cfg_write16` を足す。上位の Status は W1C)。
- 表は静的 (`kernel/kernel.c`、最初は 82557 の 1 行)。`pci_bind_all` は **pgalloc と DMA プールの初期化の後**。
  §3 の動的読み込みが来たら `size` で版を確かめる。

### 1-5. µs 時計 — `sys_time_now(u32 *lo, u32 *hi)`

- **時間源**: `tick_count` (1-0 の後は 10ms) + PIT ch0 のラッチ読み (mode 2、再ロード後は `reload` から 1 まで減る)。
  `us = tick × 10000 + ((reload − count) × 10000) / reload`。乗算は u64 (GNU89 の `unsigned long long`、カーネル内のみ、
  libgcc は既にリンク済み) で行う。
- **スナップショット手順 (R8)**、全体を `irq_save` (IF=0) の中で:
  1. `p1` = PIC1 の IRR bit0 (OCW3 = 0x0A を書いてから読む。既定選択も IRR だが明示する。`irq_finish` の ISR 読みは
     IF=0 の外では走らないので競合しない)
  2. ラッチ → `count` (下位 → 上位)
  3. `p2` = IRR bit0 を再読
  4. `t` = `tick_count` (IF=0 の中なので手順の間は不変)
  5. 判定: `p1 == 0 && p2 == 0` → `t`、`count` を採用。`p1 == 1` → 再ロードはラッチより前 → `t + 1`、`count` を採用
     (count は新周期)。`p1 == 0 && p2 == 1` → 再ロードがラッチの前後どちらか不明 → **やり直し** (最大 3 回。
     1 回は数 µs なので同じ周期の中でもう 1 度境界を踏むことは無い)。3 回失敗で `-EAGAIN`。
  これで往復 2 の反例 1 (古い count に +1) は起きない。
- **契約**: この時計が正しいのは **IRQ0 が失われない範囲**、すなわち**システム全体で IF=0 の区間が 1 周期 (10ms) 未満**
  のとき (`tick_count` 自体が同じ前提)。2 回以上の境界を IF=0 で跨いだ (反例 2) 場合は tick が 1 つ失われ、この時計も
  `tick_count` も 10ms 遅れる — 検出はしない (契約に明記、W4 で「IF=0 10.2ms」の挙動を記録)。debug ビルドは
  `irq_save` の最長区間を測る (`irq_off_max_us`)。
- **返却**: 0 = 成功、`-EAGAIN` = 3 回失敗、`-ENODEV` = `pit_setup` 未初期化または mode 2 でない。負のときは
  `*lo`/`*hi` を触らない。呼び手は `tick_count × 10000` に落とせるが、**その値は同じ tick の補間値より小さいので、
  fallback は単調性の保証に含めない** (呼び手が前回値と max を取る)。
- **KAPI**: 64 ビットの戻り値は使えない (往復 1 の B14) ので**出力引数 2 本**で末尾追記 (版数 58 → 59)。
  CPL=3 のポインタ検査は既存の作法。同一スナップショットの上下を書く。
- 単調性: u64 に組み立てるので 71 分の桁あふれは無い。`tick_count` の u32 周回 (497 日) は扱わない (契約)。

## 2. 顧客と順序

| 順 | 顧客 | 使う層 |
|---|---|---|
| 0 | 全部 (実機の時間) | 1-0 (sysclk_detect + PIT のクロック修正) — **最初に単独で着地** |
| 1 | 82557 (L-B) | 1-1 (共有 IRQ)、1-3 (CB/RFD)、1-4 |
| 2 | CS4231 PCM (§5-5 P1) | 1-2 (#1/#3、CYCLIC、remaining + TC)、1-3 (16KB)、1-1 |
| 3 | LGY-98 | 1-1 (3/5/6 をこの票で移す、アダプタ) |
| 4 | FDC | 1-2 (薄い呼び出しに置き換え、負を報告) |

## 3. 受入

| ID | 見るもの | 手段 |
|---|---|---|
| W0 | 1-0: NP21/W (1.9968MHz 設定) で reload = 19968 のまま、`kselftest` に `pit_setup` の検査を足す。**実機**: `sys_time_now` はまだ無いので、ホストの単調時計で **`tick_count` を 60 秒間隔で 2 回読む** (シリアル越し、往復遅延 < 50ms を差し引き、誤差 ±0.5% 以内。修正前は 23% ずれる)。回帰: `cpu_calibrate` の `loops_per_tick`、`cpu_delay_us(1000)` **× 1000 回の累積時間** (単発は往復遅延に埋もれる)、FD 起動、シリアル 115200 の ack | NP21/W + 実機 |
| W1 | 純粋関数のホスト試験 (変異つき): 集約と 2 巡 (同時要因、2 巡目の回収、`handled_any` の保持、tick 回収の判定、ストーム閾値)、`irq_finish` の **EOI 送信列** (master / slave / IRQ15 スプリアスの ISR 検査を handled と独立に)、登録の一括有効化と拒否規則 (非対応 IRQ、SHARED 不一致、5 件目)、`dma_split_addr` / `dma_crosses_64k` / `bytes` の範囲 / `dma_accept_pair` / **TC の read-clear の保存**、プールの最初適合と整列と 64KB 跨ぎ、**隣接 span の free と途中ポインタ・二重解放**、PCI の一致規則 (任意 / 完全 / DECLINE で次へ / QUARANTINE で打ち切り / 複数装置)、時計の整数式と **p1/p2 の判定表** (両クロック、周期境界) | `check-par` |
| W2 | FDC が `dma8237` 経由でも **NP21/W の 2HD 起動と md5 一致**、**書き込み → 読み戻しの一致**、`dma_chan_setup` が負のとき FDC コマンドを発行しない、読み失敗のタイムアウト → `fdc_abort_transfer` → 再試行。**実機の FD 起動** と 0439h の 3 値の表示 | NP21/W + 実機 |
| W3 | 共通スタブに `irq_register` した偽装置 (`kselftest` / 試験用 KAPI): NP21/W の `/api/pic` で IRR/ISR を見ながら、master (3) と slave (9) の両方、連続、共有 (2 登録の集約と**残件の tick 回収の完了**)、共有者の**片方解除**後にもう片方が動く、解除後に callback が走らない、CPL=3 のアプリ実行中と V86 中、**要因を落とさない偽装置が `DEFERRED` を返し続けて tick 回収が続く** (エッジでは 200 回の IRQ にならない) ことと、**誰も受けないエッジを 200 回/tick 打ってマスクが入る**ことを別の試験に、**残った側が `DEFERRED`・装置マスク中のまま片方を解除しても回収が続く**こと、IRQ0 の予算 2ms で持ち越しとラウンドロビンが起きること、**LGY-98 をアダプタに移した後の rshell/LAN の通信** | NP21/W |
| W4 | `sys_time_now`: 1 万回連続読みで逆行 0、両クロック (NP21/W 1.9968 / 実機 2.4576)、**境界注入**: IF=0 で PIT の残りが 1〜2 count の位相から読む試験 (`kselftest` が count を見て待ち合わせる) で p1/p2 の 3 分岐を全部踏む、IF=0 を 10.2ms 続けた後の読み (契約外の挙動を記録)、3 回失敗の返却、CPL=3 から 64 ビットが揃って返る | NP21/W + 実機 |
| W5 | カーネル増分 ≤ 5KB、内訳: IRQ 表 8 × 4 × 12B = 384B、poll/storm ビット + IRQ 別の tick 内発生数・連続 poll 回数・stall 回数 (8 × 3 × 4B = 96B)、pool ビットマップ + span 16 × 4B = 72B、DMA チャネル状態 4 × 16B、pit_setup 20B、コード (irq / dma8237 / dma_pool / pci_bind / sys_time ≒ 2.5KB、u64 の割り算は libgcc に既にある)、診断文字列 ≒ 0.5KB。`__bss_end` 差分と `docs/02_memory.md` の生成 (**新配置 0x2E8000〜0x2F7FFF が `MM_RW` で USER 無し、上下が NP**)、プールの枯渇 → 解放 → 再利用 | `docs/02_memory.md` + kselftest |
| W6 | **土台完了**の受入はここまで。82557 での連続 TX/RX・共有・再開は **L-B の受入**。HAL 単体で未検証の実機条件を残件に明記: 82557 の実 IRQ 番号での共有、PCI バスマスタのキャッシュ整合 (W7)、CS4231 の DMA (§5-5)、**NE2000 の `wait_rdc` / `hw_reset` が ISR 文脈で回る既存の契約違反 (L-C で foreground へ)** | — |
| W7 | **キャッシュ整合 (実機)**: CPU がパターンを書いて**フラッシュせずに** DMA で読ませる (FDC 書き込み → 読み戻し、82557 は L-B のループバック)、DMA で受けた直後に CPU が読む、を双方向で 100 回。所有権の移譲は「CPU 書き → 装置へ渡す → 装置完了の証拠 → CPU 読み」の順で、明示のフラッシュ命令は使わない。不一致が出たら `wbinvd` (486+) を DMA 前に入れる方針に切り替える (票を更新) | 実機 |

## 4. しないこと

- BAR の自前割り当て (BIOS 未割り当ての装置)。
- 既存の固定 IRQ (0/1/2/4/7/11/12/13) の移行。レベルトリガの PIC 設定。
- 境界モード `0029h` の変更、拡張バンク (16MB 超)、8237 の demand / block モード、番地減分。
- 1kHz tick と `pit_init` の汎用 hz (§5-5 の別票)。
- 外部モジュールの ABI (§3)。`struct pci_driver.size` はそのための口だけ。

## 5. ユーザー決裁が要る点

> **決裁 (ユーザー、2026-09-23)**: (1) DMA プールの 0x2E8000 固定は**暫定承諾** (v3 でメモリマップ自体を見直す予定。
> その時に再配置し得る)。(2) 1-0 の先行着地は**承認**、先に直す。レビューは追加 1 往復 (往復 4) を使う。

1. **DMA プールを SQLite 帯の予約域 0x2E8000〜0x2F7FFF に固定** (メモリマップの数字。決裁 D1 の続き)。
   SHM・GUI オフセット・SDK・アプリは無変更。代償は SQLite の成長余地が 120KB → 44KB になること (リンク時 ASSERT)。
2. 1-0 (`sysclk_detect` + PIT のクロック修正) を**この票より先に単独で着地**させてよいか (実機の時間が 23% ずれている
   既存バグ。W0 を単独で閉じる)。

## 6. この票の外で見つけた不整合

- `tools/memmap_audit_live.py:25` は GUI 基点を `MEM_SHM_BASE + 0x30000` で決め打ちしている。正典 (`MEM_SHM_GUI_OFFSET`
  = 0x28000) とずれている (2026-09-17 の末尾相対化の取り残し)。別の小票で直す。
