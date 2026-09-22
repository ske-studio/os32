# TASK_PCM_CS4231 — CS4231 (MATE-X PCM) の PCM 再生ドライバ (§5-5 の P1)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 v2 (Codex 往復 1 の 15 件を反映、往復 2 待ち)**。
> 正典の関係: [`PLAN.md`](PLAN.md) §5-5、土台は [`TASK_HAL_WIRING.md`](TASK_HAL_WIRING.md) (1-1 割り込み、1-2 8237、1-3 プール、1-5 時計)、
> 出力保護は [`../memory/TASK_KAPI_OUTPUT_GUARD.md`](../memory/TASK_KAPI_OUTPUT_GUARD.md)。
> 典拠: Crystal **CS4231A データシート DS139PP2** (`docs/hw/crystal/cs4231a.pdf`、gitignore のミラー、`pdftotext` 済み)、
> `docs/hw/undocumented/io_sound.md`「PC-9821X･N内蔵型」、NP21/W `src/sound/cs4231c.c` / `src/cbus/cs4231io.c`。
> **実機不要**: NP21/W が CS4231 を再現する。実機 (Ra266) は E6 で同じコードを通す。

## 0. 範囲

**P1 だけ**: 16 ビット・ステレオ・44.1kHz (と 22.05kHz) の**再生**を、カーネル所有の DMA リングと KAPI で提供する。
単位は **frame** (左右 1 組 = 4 バイト)。録音・ミキサ (出力減衰以外)・PIO・V86 への提供 (P4)・ソフト合成器 (P2) は含まない。

## 1. ハードウェアの事実 (典拠つき)

| 項目 | 事実 |
|---|---|
| 検出 | **`0F43h` の WSS ID (下位 6 ビット = 000100b) を一次条件**にし、Index 12 (MODE and ID) の ID3-0 = 1010、I25 の V2-0 = 100/101 で確認する (DS139PP2 p.36/38)。`A460h` のサウンド ID は**参考情報**: NP21/W の `SNDboard=0x64` (86 + MATE-X PCM) では PCM の ID ポートが **B460h** に移る (`cs4231io.c:261`) ので、A460h を必須にすると NOSYS になる (往復 1 B1)。0C2Bh/0C2Dh の再配置はしない (既定 0F40h〜) |
| ポート | `0F44h` R0 Index (INIT / MCE / TRD / IA4-0)、`0F45h` R1 Data、`0F46h` R2 Status (INT bit0、**どんな値でも書けば INT が消える**)、`0F47h` R3 PIO (使わない) |
| 割り込みと DMA | **`0F40h`**: bit5-3 = INT (001 INT0 = IRQ3 / 010 INT2 = IRQ6 / 011 INT41 = IRQ10 / 100 INT5 = IRQ12)、bit2-0 = DMA (001 #0 / 010 #1 / 011 #3、111 = 無し。NP21/W の表では 111 も #3)。NP21/W: `cs4231irq[] = {ff,03,06,0a,0c,…}`、`cs4231dma[] = {ff,00,01,03,…}`。**書くとその場で DMA の attach が変わり、NP21/W はアドレスを先頭に戻す** (`cs4231io.c:379`) |
| 選ぶ値 | **INT41 (IRQ10) + DMA #1 → `0F40h = 0x1A`**。IRQ10 は 1-1 の動的な線、LGY-98 の既定 (3/5) と FDC (#2) を避ける。IRQ12 は固定スタブ (V86)。登録に失敗したら**利用不可** (1-1 の決定) |
| **I8 (Fs & Playback Data Format)** | D7-D5 = FMT1:FMT0:C/L、D4 = S/M、D3-D1 = CFS、D0 = C2SL。**16 ビット LE 2 の補数 = 010、ステレオ = 1** → 上位 nibble **0x5**。44.1kHz = XTAL2 (16.9344MHz、C2SL=1) ÷384 = CFS 5 → **I8 = 0x5B**。22.05kHz = XTAL2 ÷768 = CFS 3 → **0x57** (DS139PP2 p.33)。往復 1 B2 の 0x9B は予約フォーマット。**XTAL2 が無い機械では 44.1k/22.05k は出ない** (XTAL1 だけなら 48/32/16k) — 実機で I17 の XTALE と実際の周波数を E6 で確かめる |
| I9 (Interface Configuration) | D7 CPIO / D6 PPIO / D4-3 CAL / D2 SDC / D1 CEN / **D0 PEN**。CEN/PEN 以外は **MCE 中しか書けない** (p.34)。使う値: MCE 中に `0x08` (CAL = 2 = DAC calibration、PPIO=0、SDC=0、PEN=0)、稼働は `PEN=1` を MCE 無しで on-the-fly |
| I10 (Pin Control) | **D1 = IEN** (割り込みピン許可)。他は 0 |
| I11 (Error Status、読み取り専用) | **D5 = ACI** (校正中)、D6 = PUR (再生アンダーラン、R2 読みで消える)。往復 1 B4 の「bit7」は誤り |
| **I12 (MODE and ID)** | **D6 = MODE2**。I16〜I31 (I24 を含む) は MODE2 でだけ見える。MODE1 のまま Index 24 を選ぶと IA4 が無視されて **I8 に化ける** (往復 1 B3)。初期化の最初に MODE2 を立て、読み戻して確認する |
| **I14 / I15 (Playback Base)** | 16 ビット = **NS − 1** (NS = 割り込み間の frame 数。16 ビットステレオでも「サンプル」= frame、p.24)。**I15 (下位) → I14 (上位) の順に書く。上位の書き込みが Current Count をロードする** (往復 1 B5) |
| I16 (Alternate Feature Enable I) | D0 DACZ (アンダーラン時に 0 を出す。**1 にする**)、D4 PMCE。I17 D1 XTALE |
| **I24 (Alternate Feature Status)** | D4 = **PI** (再生 DMA カウントの割り込み)、D0 PU。**PI は「そのビットに 0 を書く」か「R2 に何か書く」で消える** (p.39、往復 1 非 blocker)。NP21/W は IEN=0 のあいだ PI 自体を立てない (`cs4231c.c:474`) ので IEN=0 での PI 観測は実機と同じにならない |
| INIT / MCE / 校正 | INIT 中と MCE 後のクロック再同期中は**全読みが 0x80、書きは無視** (p.19-21)。手順: MCE を立てる → I8 を 1 回の書きで変える → **R0 が 0x80 でなくなるまで poll** → I9 を書く → MCE を落とす → **ACI (I11 D5) が落ちるまで poll** (CAL=2 は 40 サンプル周期 ≒ 1ms)。待ちは全部 **IF=1 で tick の期限つき** (IF=0 の一括ロックの中に置かない) |
| PC-98 の 8237 | 1-2 の `dma_chan_setup(1, ring, 16384, DMA_DIR_FROM_MEM, DMA_MODE_CYCLIC)` (auto-init)。リングは 1-3 のプールが 64KB 跨ぎを保証。NP21/W の折り返しは専用関数が無条件にアドレスを戻すので **auto-init ビットが正しい証明にはならない** (E6 で実機) |

## 2. 設計

### 2-1. カーネル `drivers/pcm_cs4231.c` (+ `pcm_cs4231_math.c` 純粋部)

**レジスタアクセス**: `cs_read(idx)` / `cs_write(idx, val)` は **Index と Data の組を 1 つの `irq_save` の中**で行う (往復 1 B9: foreground が I6 を選んだ直後に IRQ が I24 を選ぶと音量が I24 に入る)。IRQ handler も同じ関数を使う (IF=0 なので入れ子は無い)。MCE の列の途中 (`s_mce_busy = 1`) は IRQ handler と tick は装置に触らない (PI は IEN=0 なので出ない)。

**リング**: 1-3 の `dma_pool_alloc(16384, 4096)` = 4096 frame = **半バッファ 2048 frame × 2** (46ms × 2)。Playback Base = 2047 → 半バッファごとに PI。open 時に**リング全体を 0 で埋める** (往復 1 B8)。

**状態機械**: `CLOSED → OPENING → OPEN (PEN=0、DMA マスク) → RUNNING → DRAINING → STOPPING → CLOSED`、失敗の終端 `FAULTED` (再 open は `OS32_ERR_IO`、再起動まで)。IRQ handler と tick は状態を見て動く (CLOSED / OPENING / FAULTED では何もしない)。

**open の列と巻き戻し** (往復 1 B6/B12): (1) 状態 CLOSED を確認、OPENING に → (2) 検出 (1 の表) → (3) `dma_pool_alloc` → (4) **`0F40h = 0x00`** (INT/DMA を外す) → R2 に書いて INT を消し、PEN=0 / IEN=0 を書く (BIOS の旧設定の要因を止める) → (5) `dma_chan_mask(1)` → (6) `irq_register(10, pcm_irq, 0, IRQ_F_SHARED)` → (7) 初期化列 (下) → (8) **`0F40h = 0x1A`** (経路を結ぶ。NP21/W はここでアドレスを戻すので DMA setup より前) → (9) `dma_chan_setup(1, …, CYCLIC)` (マスクのまま) → (10) I15 = 0xFF、I14 = 0x07 → (11) owner を公開、状態 OPEN。**失敗は逆順**: (7)〜(10) の失敗は `0F40h = 0` → `irq_unregister` → `dma_pool_free` (DMA 未開始なので安全) → CLOSED、戻り値は段階ごと (`OS32_ERR_NOSYS` 検出なし / `OS32_ERR_NOMEM` プール / `OS32_ERR_BUSY` IRQ 登録 / `OS32_ERR_IO` 初期化の期限)。

**初期化列** (1 の表の典拠どおり、各 poll は IF=1・期限 5 tick、超えたら `OS32_ERR_IO` で巻き戻し): R0 が 0x80 でなくなるまで poll (INIT) → I12 に MODE2=1 を書き、読み戻して MODE2 を確認 → R0 = MCE | 8、I8 = 0x5B (または 0x57) → R0 が 0x80 でなくなるまで poll → I9 = 0x08 (CAL=2) → R0 = 8 (MCE 落とす) → I11 の ACI が落ちるまで poll → I16 に DACZ=1 → I6/I7 = 0 (0dB、ミュート解除) → I24 に 0 を書いて PI/PU を消す → I10 の IEN=1。

**RUNNING への遷移** (往復 1 B10): 最初の `pcm_write` で **前半 (half 0) が満ちた**とき、または `pcm_close` で未転送データがあるとき (残りは 0 のまま) に、`dma_chan_unmask(1)` → I9 の PEN=1 (MCE 無しで書ける)。

**進行の一元化 `pcm_advance()`** (往復 1 B7): IRQ handler と tick フックの**両方がこれだけ**を呼ぶ。中身: (a) I24 を読み、PI があれば I24 に 0 を書いて消す (handled = 1)、(b) 1-2 の `dma_chan_remaining(1)` で装置の位置 `pos` (リング内 frame) を取る、(c) `pos` が前回と違う半分にいれば「半分が切り替わった」: 切り替わった回数 `gen` を +1、**消費された半分を即 0 で埋める** (往復 1 B8)、その半分にアプリが書いた `fill[half]` を 0 に戻す。アプリが埋める前に切り替わった (`fill[half] < 2048` だった) なら `underruns` +1。(d) remaining が 3 回続けて失敗、または `pos` が 2 回の観測で同じ半分のまま **2 半周期 (93ms) を超えた**のに PEN=1 なら `RESYNC`: `dma_chan_mask` → リングを 0 に → `dma_chan_setup` し直し → I15/I14 を書き直す → unmask、`resyncs` +1。観測は tick (10ms) で保証するので半周期 (46ms) の中で必ず 1 回以上見る。**周回数は remaining からは復元しない** (1-2 の契約): 見えたのは「今どの半分か」だけで、取りこぼしは underrun として数える (2 周を 1 周と誤るのは許容し、resyncs / underruns の数を KAPI で見せる)。

**`pcm_write` と公開規則** (往復 1 B8/B14): 受け付けるのは **frame の倍数** (`bytes & 3` は切り捨て、0 なら 0 を返す)。書く先は「装置が読んでいない半分」の `fill[half]` 以降だけ、その半分の残り (`2048 − fill`) までを受ける (**最大 8KB のコピー**)。入力の検査は `ring3_user_range_ok(buf, bytes)` (+ 加算あふれ) を **wrapper が先に**行い、コピーは **IF=1** で 1KB ごとに `fill` を進める (途中の #PF は既存のフォールトガードから `exec_exit` → 回収へ。`fill` は完了した分だけ進んでいる)。装置がコピー中にその半分に入ってしまったら、未到達の部分は 0 (open 時と切り替え時のゼロ化) が鳴る = `late_fills` +1 (音は途切れるが壊れない)。**アプリのバッファを IRQ から読むことはしない**。満杯 (その半分が 2048 まで埋まっている) なら 0 を返し、アプリは `sys_yield` して再試行する (yield は GUI では park、CUI/CPL=0 では hlt 1 回。driver の中で yield はしない)。

**`pcm_close` = drain、期限つき** (往復 1 B10/B11): DRAINING に → 最後に書いた frame を装置が**通り過ぎ、さらに半周期ぶん (46ms) 進む** (DMA 完了と DAC 出力は別: 残りの 0 が流れて FIFO が空になる) まで IF=1 で待つ (期限 = 3 半周期 + 1 tick = 150ms)。期限切れなら abort と同じ手順。停止の列 (STOPPING): (1) IEN=0 → (2) PEN=0 → (3) R2 に書いて INT を消し、I24 に 0 を書く → (4) `dma_chan_mask(1)` → (5) **停止の証拠**: I9 を読んで PEN=0、I24 の PI=0、R0 が 0x80 でない (装置が生きて答えている) を 5 tick 以内に確認。証拠が取れたら `irq_unregister` → 状態 CLOSED (tick はもう触らない) → `dma_pool_free`。取れなければ `dma_pool_mark_leaked` → **FAULTED** (再 open 不可)。**「PI が来ない」は証拠にしない**。

**所有と回収** (往復 1 B13): owner は **現在のアプリ ID** (`g_cur_app` の id。exec のネスト段ではない)。`pcm_write / status / close / set_volume` は owner 一致を要求 (不一致は `OS32_ERR_PERM` 相当の負値 — 既存に無ければ `OS32_ERR_INVAL`)。回収は `exec_reclaim_owned(id)` に `pcm_reclaim(id)` を足す: **待たない abort** (IEN=0 → PEN=0 → ack → mask → 証拠の読み戻し 1 回 → unregister → free、証拠が無ければ leaked/FAULTED)。正常終了・fault・CTRL+STOP・park 中の kill で同じ経路。CPL=0 の常駐シェルからは owner = 0 で同じ口。

**レート**: 44100 / 22050 だけ。**音量**: I6/I7 の減衰 (0〜63 × 1.5dB) を `percent` (0〜100、100 = 0dB、0 = ミュート) から線形に写す。101 以上は `OS32_ERR_INVAL`。

### 2-2. KAPI (v61、末尾追記。出力引数は出力保護つき、入力は `ring3_user_range_ok`)

```c
int  pcm_open(u32 rate);                          /* 0 / NOSYS / BUSY (他の owner が open 中) / INVAL (rate) / NOMEM / IO (初期化の期限) */
int  pcm_write(const void *buf, u32 bytes);       /* 受け取ったバイト数 (frame の倍数、0 = 満杯) / 負 = 未 open・非 owner・範囲外 */
int  pcm_status(u32 *free_bytes, u32 *counters);  /* counters = underruns<<16 | late_fills<<8 | resyncs (各 8/8/16 ビット、飽和)。出力 2 本、1-5 と同じ保護 */
int  pcm_close(void);                             /* drain、期限つき。0 / 負 (abort に落ちた = OS32_ERR_IO) */
int  pcm_set_volume(u32 percent);
```

### 2-3. 純粋関数 (ホスト試験)

`pcm_advance` の判定 (pos → half、切り替え検出、underrun / late_fill の計数、RESYNC の条件)、`pcm_write` の受け入れ長 (frame 倍数・空き・半分の境界)、レート → I8 の値、percent → 減衰、初期化列と停止列を**ポート書きの列**として返して照合 (順序の変異: I14 → I15、MCE 無しの I9、MODE2 無しの I24)、状態機械の遷移表と不正遷移。

## 3. 受入

| ID | 見るもの | 手段 |
|---|---|---|
| E0 | **前提**: NP21/W の `/api/sound` に CS4231 を足す: PEN / IEN / MODE2 / I8 / Base / `bufdatas` / **PI の累積回数** (NP21/W の `totalsample` は周期内残量なので新設) / PU 回数 / DMA ch と IRQ / 0F40h の値、および **直近 1 秒の出力 PCM (16 ビットステレオ) を base64 で返す `?pcm=1`** (`cs4231_getpcm` にリングを足す)。NP21/W は自前フォーク (`make build && make deploy` は NP21/W 停止が要る) | np21w-src |
| E1 | ホスト試験 (変異つき): 2-3 の全部 | `check-par` |
| E2 | NP21/W (**別 trial ini で `SNDboard=0x64` = [D2]**): 検出 (0F43h + I12 + I25) → 起動行 `[pcm] CS4231 v=101 irq 10 dma 1 fmt 0x5B` | NP21/W |
| E3 | CPL=3 の `pcm_test`: **左 = 1kHz 正弦、右 = frame 番号の下位 16 ビット** (左右取り違え・旧半分の反復・ミュートを見分ける) を 5 秒 → E0 の PCM を取って左右の内容を照合、PI 累積 ≒ 5 × 44100 / 2048、underruns 0、`pcm_status` の free が書き込みで 4 バイト単位に減り**半分の解放で 8KB 戻る**。書き込みを 200ms 止めて無音 (underruns +) → 再開後に右チャネルの番号が飛ばずに続く | NP21/W |
| E4 | 共有 IRQ の実証 (証明範囲を明記): PCM だけ / 偽装置だけ / 同時 (PI と偽要因を同じ tick に) / 2 巡目の回収 / 偽装置の tick 回収。偽装置は 2 つ目の物理要因ではない (電気的共有は実機の 82557 で) | NP21/W |
| E5 | CPL=3・CUI の `pcm_test` を CTRL+STOP で殺す → `pcm_reclaim` で PEN=0・DMA マスク・次の open が通る。GUI の park 中の kill と CPL=0 は対象外と明記 | NP21/W |
| E6 | 実機 (Ra266) — 独立した受入: MODE1 の初期状態、INIT 中の書き無視、MCE / 校正の待ち、Base 上位書きのロード、DAC の初期ミュート、**XTAL2 の有無** (44.1k が出るか)、BIOS の 0F40h 旧値、1MB 超 DMA、DRQ/FIFO の停止タイミング、auto-init ビット、実際の音 (耳) | 実機 |

## 4. しないこと

録音、PIO、8 ビット、モノラル (アプリ側で複製)、ミキサの入力側、86 型 PCM の FIFO の模倣 (P4)、`0C2Bh/0C2Dh` の再配置、DMA #0/#3、I8 の XTAL1 系レート (48k 等。XTAL2 が無い機械への対応は E6 の結果で決める)。

## 5. 決裁が要る点

1. NP21/W の trial ini の複製に `SNDboard=0x64` (PC-9801-86 + MATE-X PCM、既存の OPN も残る) を入れる ([D2]。原本は触らない)。
2. E0 の NP21/W フォーク拡張は NP21/W の停止 → 配備 → 起動を伴う ([D1] と同じ型)。
