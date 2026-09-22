# TASK_PCM_CS4231 — CS4231 (MATE-X PCM) の PCM 再生ドライバ (§5-5 の P1)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 v4 (Codex 往復 3 の 8 件を反映: ステージング方式に変更、tick 駆動の RESYNC/STOPPING。往復 4 待ち)**。
> 正典の関係: [`PLAN.md`](PLAN.md) §5-5、土台は [`TASK_HAL_WIRING.md`](TASK_HAL_WIRING.md) (1-1 割り込み、1-2 8237、1-3 プール、1-5 時計)、
> 出力保護は [`../memory/TASK_KAPI_OUTPUT_GUARD.md`](../memory/TASK_KAPI_OUTPUT_GUARD.md)。
> 典拠: Crystal **CS4231A データシート DS139PP2** (`docs/hw/crystal/cs4231a.pdf`、gitignore のミラー、`pdftotext` 済み)、
> `docs/hw/undocumented/io_sound.md`「PC-9821X･N内蔵型」、NP21/W `src/sound/cs4231c.c` / `src/cbus/cs4231io.c`。
> **NP21/W 上の実装と一次受入 (E0〜E5) は実機不要**。実機でしか検証できない項目は E6 に残る。

## 0. 範囲

**P1 だけ**: 16 ビット・ステレオ・44.1kHz (と 22.05kHz) の**再生**を、カーネル所有の DMA リングと KAPI で提供する。
単位は **frame** (左右 1 組 = 4 バイト)。録音・ミキサ (出力減衰以外)・PIO・V86 への提供 (P4)・ソフト合成器 (P2) は含まない。

## 1. ハードウェアの事実 (典拠つき)

| 項目 | 事実 |
|---|---|
| 検出 | **一続きの手順** (往復 2 R1: MODE1 のまま I25 を選ぶと I9 に化けて正常な装置を NOSYS にする): (a) `0F43h` の WSS ID (下位 6 ビット = 000100b) → (b) R0 が 0x80 でなくなるまで待つ (INIT) → (c) I12 の ID3-0 = 1010 を確認し、**MODE2 を書いて読み戻す** → (d) I25 の V2-0 = 100 (CS4231) / 101 (CS4231A) を確認 (DS139PP2 p.36/38)。`A460h` のサウンド ID は**参考情報**: NP21/W の `SNDboard=0x64` (86 + MATE-X PCM) では PCM の ID ポートが **B460h** に移る (`cs4231io.c:261`) ので、A460h を必須にすると NOSYS になる (往復 1 B1)。0C2Bh/0C2Dh の再配置はしない (既定 0F40h〜) |
| ポート | `0F44h` R0 Index (INIT / MCE / TRD / IA4-0)、`0F45h` R1 Data、`0F46h` R2 Status (INT bit0、**どんな値でも書けば INT が消える**)、`0F47h` R3 PIO (使わない) |
| 割り込みと DMA | **`0F40h`**: bit5-3 = INT (001 INT0 = IRQ3 / 010 INT2 = IRQ6 / 011 INT41 = IRQ10 / 100 INT5 = IRQ12)、bit2-0 = DMA (001 #0 / 010 #1 / 011 #3、111 = 無し。NP21/W の表では 111 も #3)。NP21/W: `cs4231irq[] = {ff,03,06,0a,0c,…}`、`cs4231dma[] = {ff,00,01,03,…}`。**書くとその場で DMA の attach が変わり、NP21/W はアドレスを先頭に戻す** (`cs4231io.c:379`) |
| 選ぶ値 | **INT41 (IRQ10) + DMA #1 → `0F40h = 0x1A`**。IRQ10 は 1-1 の動的な線、LGY-98 の既定 (3/5) と FDC (#2) を避ける。IRQ12 は固定スタブ (V86)。登録に失敗したら**利用不可** (1-1 の決定) |
| **I8 (Fs & Playback Data Format)** | D7-D5 = FMT1:FMT0:C/L、D4 = S/M、D3-D1 = CFS、D0 = C2SL。**16 ビット LE 2 の補数 = 010、ステレオ = 1** → 上位 nibble **0x5**。44.1kHz = XTAL2 (16.9344MHz、C2SL=1) ÷384 = CFS 5 → **I8 = 0x5B**。22.05kHz = XTAL2 ÷768 = CFS 3 → **0x57** (DS139PP2 p.33)。往復 1 B2 の 0x9B は予約フォーマット。**XTAL2 が無い機械では 44.1k/22.05k は出ない** (XTAL1 だけなら 48/32/16k) — 実機で I17 の XTALE と実際の周波数を E6 で確かめる |
| I9 (Interface Configuration) | D7 CPIO / D6 PPIO / **D4:D3 = CAL1:CAL0** / D2 SDC / D1 CEN / **D0 PEN**。CEN/PEN 以外は **MCE 中しか書けない** (p.34)。使う値: MCE 中に **`0x08` = CAL 1 (Converter calibration、136 サンプル周期 ≒ 3.1ms @44.1k)** — CS4231 (V=100) は CAL1 が予約なので両版に共通な値を採る (往復 2 R2。DAC だけの校正 `0x10` は CS4231A 専用)。PPIO=0、SDC=0、PEN=0。稼働は `PEN=1` を MCE 無しで on-the-fly |
| I10 (Pin Control) | **D1 = IEN** (割り込みピン許可)。他は 0 |
| I11 (Error Status、読み取り専用) | **D5 = ACI** (校正中)、D6 = PUR (再生アンダーラン、R2 読みで消える)。往復 1 B4 の「bit7」は誤り |
| **I12 (MODE and ID)** | **D6 = MODE2**。I16〜I31 (I24 を含む) は MODE2 でだけ見える。MODE1 のまま Index 24 を選ぶと IA4 が無視されて **I8 に化ける** (往復 1 B3)。初期化の最初に MODE2 を立て、読み戻して確認する |
| **I14 / I15 (Playback Base)** | 16 ビット = **NS − 1** (NS = 割り込み間の frame 数。16 ビットステレオでも「サンプル」= frame、p.24)。**I15 (下位) → I14 (上位) の順に書く。上位の書き込みが Current Count をロードする** (往復 1 B5) |
| I16 (Alternate Feature Enable I) | D0 DACZ (アンダーラン時に 0 を出す。**1 にする**)、D4 PMCE。I17 D1 XTALE |
| **I24 (Alternate Feature Status)** | D4 = **PI** (再生 DMA カウントの割り込み)、D0 PU。**PI は「そのビットに 0 を書く」か「R2 に何か書く」で消える** (p.39、往復 1 非 blocker)。NP21/W は IEN=0 のあいだ PI 自体を立てない (`cs4231c.c:474`) ので IEN=0 での PI 観測は実機と同じにならない |
| INIT / MCE / 校正 | INIT 中と MCE 後のクロック再同期中は**全読みが 0x80、書きは無視** (p.19-21)。**MCE を落とした後にも 0x80 が現れる** (XTALE=0 で水晶を切り替えたとき、p.21) ので、I11 の読み値 0x80 を「ACI=0」と読んではいけない (往復 2 R3)。手順: MCE を立てる (R0 = 0x40 \| 8) → I8 を 1 回の書きで変える → **R0 が 0x80 でなくなるまで poll** → I9 を書く (**MCE を保ったまま**: `cs_write_mce(9, v)` は R0 に 0x40 \| 9 を書く。素の `cs_write` は R0 に idx だけを書くので MCE が落ちる) → MCE を落とす (R0 = 9) → **もう 1 度 R0 が 0x80 でなくなるまで poll** → **ACI (I11 D5) が落ちるまで poll** (136 周期 ≒ 3.1ms)。待ちは全部 **IF=1 で tick の期限つき** (5 tick、IF=0 の一括ロックの中に置かない) |
| PC-98 の 8237 | 1-2 の `dma_chan_setup(1, ring, 16384, DMA_DIR_FROM_MEM, DMA_MODE_CYCLIC)` (auto-init)。リングは 1-3 のプールが 64KB 跨ぎを保証。NP21/W の折り返しは専用関数が無条件にアドレスを戻すので **auto-init ビットが正しい証明にはならない** (E6 で実機) |

## 2. 設計

### 2-1. カーネル `drivers/pcm_cs4231.c` (+ `pcm_cs4231_math.c` 純粋部)

**方式の変更 (往復 3 B2/B3)**: アプリのデータを foreground から DMA リングへ直接コピーする方式は、装置の半分の切り替えや
RESYNC との競合を `gen` の検査では消せない (既に鳴ったデータは取り消せない) ので**やめる**。**foreground が書くのは
ステージング (DMA しない 16KB) だけ**、**DMA リングを書くのは `pcm_advance` (IRQ / tick、IF=0) だけ**にする。
`pcm_advance` は「装置が別の半分に移った」ことを観測した直後に、**消費し終えた半分**へステージングから写す (装置はもう
片方の先頭にいるので 1 半周期 = 46ms の余裕。書き手は 1 人、IF=0、8KB の memcpy は 386 でも 1ms 以内)。

**メモリ**: 1-3 の `dma_pool_alloc` から **リング 16KB (DMA、4096 frame = 半バッファ 2048 × 2) + ステージング 16KB (4096 frame)**
= 32KB (プール 64KB のうち。82557 の 16KB と共存できる)。open 時に両方を 0 で埋める。

**レジスタアクセス**: `cs_read(idx)` / `cs_write(idx, val)` / `cs_write_mce(idx, val)` (R0 に `0x40 | idx` を書いて MCE を保つ) は
**Index と Data の組を 1 つの `irq_save` の中**で行う (往復 1 B9)。MCE の列の途中 (`s_mce_busy = 1`) は IRQ / tick は装置に触らない。

**状態機械** (装置を触る遷移は全部 **tick 駆動の分割状態**にし、IRQ / tick の中で待たない。foreground の待ちは IF=1・期限つき):

| 状態 | advance の動作 | write | 遷移 |
|---|---|---|---|
| CLOSED | 何もしない | 拒否 | open → OPENING |
| OPENING | 何もしない (IEN=0) | 拒否 | 検出 + 初期化列 (foreground、期限つき) → OPEN / 失敗 → 巻き戻し → CLOSED |
| OPEN | 何もしない (PEN=0) | ステージングへ | ステージングが 2048 frame 以上、または close で未転送あり → `pcm_start` → RUNNING |
| RUNNING | 切り替えの観測・補充・番犬 | ステージングへ | close → DRAINING / 番犬 → RS_STOP / reclaim → STOP_REQ |
| DRAINING | 同上 (新規の write は拒否) | 拒否 | ステージングが空 + 最後のデータの半分を通過 + さらに 1 半分 (0) を通過 → STOP_REQ / 番犬 → **drain 失敗を記録して STOP_REQ** (往復 3 B4) |
| RS_STOP | IEN=0 → PEN=0 → ack (即)。以後の tick で **I11 の DRS (D4、DRQ 動作中) = 0** を待つ (期限 3 tick) → RS_RESTART。期限切れ → STOP_REQ (失敗) | ステージングへ | |
| RS_RESTART | `dma_chan_mask` → リング 0 化 → `filled[] = 0` → `dma_chan_setup` → I15/I14 → ステージングから半分 0 (と 1) へ写す → half = 0、gen +1 → unmask → PEN=1 → IEN=1 → RUNNING、`resyncs` +1 | | |
| STOP_REQ | IEN=0 → PEN=0 → R2 に書いて INT を消し、I24 に 0 (即)。以後の tick で DRS=0 を待つ (期限 3 tick) → `dma_chan_mask` → **証拠**: I9 の PEN=0、I24 の PI=0、R0 != 0x80 → STOP_DONE。証拠が無い (期限内に読めない) → FAULTED | 拒否 | |
| STOP_DONE | PI だけ ack | 拒否 | foreground (close / reclaim) が **`irq_unregister` → `dma_pool_free` (リングとステージング)** → CLOSED |
| FAULTED | PI だけ ack | 拒否 | foreground が `irq_unregister` → `dma_pool_mark_leaked` (両方)。再 open は `OS32_ERR_IO` (再起動まで) |

`irq_unregister` と `dma_pool_free` は **foreground だけ** (ISR 文脈からの解除は 1-1 で `IRQ_ERR_CTX`)。STOP_* / CLOSED / FAULTED /
OPENING では advance は PI を ack するだけ (`IRQ_HANDLED` / 無ければ `IRQ_NONE`)。**新規の write を受けるのは OPEN と RUNNING と RS_* だけ**。

**open の列と巻き戻し** (往復 1 B6/B12、往復 2 R4): (1) 状態 CLOSED を確認、OPENING に → (2) `dma_chan_mask(1)` → (3) **`0F40h = 0x1A`**
(先に新しい経路を結ぶ。DMA #1 はマスク済み、IRQ10 は未登録だが装置は IEN=0。NP21/W は `0F40h = 0` で detach して
`dmach = 0xff` になり、その状態で I8 を書くと `cs4231_control` が配列外 (`dmac.dmach[255]`) を触るので、**detach 状態で装置
レジスタを書かない**。E0 で NP21/W 側にも未 attach の防御を足す) → (4) R2 に書いて INT を消し、I9 の PEN=0 と I10 の IEN=0 を書く
(BIOS の旧設定の要因を止める) → (5) 検出 (1 の表、MODE2 まで) → (6) `dma_pool_alloc` × 2 → (7) `irq_register(10, pcm_irq, 0,
IRQ_F_SHARED)` → (8) 初期化列 (下) → (9) `dma_chan_setup(1, …, CYCLIC)` (マスクのまま) → (10) I15 = 0xFF、I14 = 0x07 → (11) owner を
公開、状態 OPEN。**失敗は逆順**: (8)〜(10) の失敗は `irq_unregister` → `dma_pool_free` × 2 (DMA 未開始・マスク中なので安全) →
PEN=0/IEN=0 のまま `0F40h` は **0x1A のまま残す** (detach しない) → CLOSED。戻り値: `OS32_ERR_NOSYS` 検出なし / `OS32_ERR_NOMEM`
プール / `OS32_ERR_BUSY` IRQ 登録 / `OS32_ERR_IO` 初期化の期限。

**初期化列** (§1 の典拠どおり。各 poll は IF=1・期限 5 tick、超えたら `OS32_ERR_IO` で巻き戻し。往復 3 B1: §1 と一致させた):
R0 が 0x80 でなくなるまで poll (INIT) → I12 に MODE2=1 を書き、読み戻して MODE2 を確認 → **`cs_write_mce(8, 0x5B)`** (44.1k。
22.05k は 0x57) → R0 が 0x80 でなくなるまで poll (クロックの再同期) → **`cs_write_mce(9, 0x08)`** (CAL 1、PPIO=0、SDC=0、PEN=0。
MCE を保つ) → R0 = 9 (MCE を落とす) → **もう 1 度 R0 が 0x80 でなくなるまで poll** → I11 の ACI (D5) が落ちるまで poll (136 周期
≒ 3.1ms) → I16 に DACZ=1 → I6/I7 = 0 (0dB、LDM/RDM = 0) → I24 に 0 (PI を消す) → I10 の IEN=1。

**`pcm_start`** (OPEN → RUNNING、foreground、装置は停止中): ステージングから半分 0 へ min(2048, staged) frame を写し
(`filled[0]`)、残りがあれば半分 1 へも (`filled[1]`)、残りは 0 のまま → `irq_save` の中で half = 0、gen = 1、状態 RUNNING を公開 →
`dma_chan_unmask(1)` → I9 の PEN=1 (MCE 無しで書ける)。

**`pcm_advance()`** (IRQ handler と tick フックの両方がこれだけを呼ぶ。往復 3 B5/B7 を反映):
1. I24 を読み、PI があれば I24 に 0 を書いて消す (handled = 1)。状態が RUNNING / DRAINING / RS_* でなければここで終わる。
2. `dma_chan_remaining(1, &left, &tc)` (1-2)。**成功なら** `pos_bytes = (16384 − left) % 16384`、`new_half = pos_bytes / 8192`、
   `last_ok = 今` (`sys_time_now`)。**`-EAGAIN` なら pos / half / filled は更新せず**、5 へ進む (番犬は失敗時も評価する)。
3. **切り替え** (`new_half != half`): `old = half`、`half = new_half`、gen +1。**判定は消す前に**: RUNNING で `filled[new_half] < 2048`
   なら `underruns` +1 (いま鳴り始めた半分が満ちていなかった)。DRAINING で `new_half` が「最後のデータの半分」より後なら
   underrun に数えない (末尾の 0 埋め)。
4. **補充**: 消費し終えた `old` へ、ステージングから min(2048, staged) frame を写し (IF=0、≤ 8KB)、残りを 0 で埋め、
   `filled[old]` = 写した数、`stg_r` を進める。RUNNING で写せたのが 2048 未満なら、その半分が鳴る番になったときに 3 で
   underrun に数えられる。DRAINING でステージングが空になったら「最後のデータの半分」= 直前に写した半分を記録。
5. **番犬** (`sys_time_now` の差、rate から計算した半周期の 2 倍): 有効な観測が無い期間、または同じ半分に留まった期間
   (最後に半分が変わった時刻から) が 2 半周期を超えたのに PEN=1 なら → RUNNING では **RS_STOP**、DRAINING では **drain 失敗を
   記録して STOP_REQ**。tick (10ms) は呼び出しの機会であって有効観測の保証ではない。

**`pcm_write`** (foreground、往復 3 B2/B3 で DMA メモリに触らない形に): 受け付けるのは **frame の倍数** (`bytes & 3` は切り捨て、
0 なら 0)。入力の検査は `ring3_user_range_ok(buf, bytes)` (+ 加算あふれ) を **wrapper が先に**行う。`irq_save` の中で
`stg_w` とステージングの空きを取り、空きまでの長さを予約 → **IF=1** でユーザのバッファからステージングの `[stg_w, stg_w + n)`
へコピー (この区間は公開前なので advance は読まない。途中の #PF は既存のフォールトガードから `exec_exit` → 回収へ。
公開していないので状態は無傷) → `irq_save` の中で `stg_w` を進めて公開 (RS_* の途中でも安全: リングの再構成はステージング
と独立)。空きが無ければ 0 を返し、アプリは `sys_yield` して再試行 (yield は GUI では park、CUI/CPL=0 では hlt 1 回。driver の
中では yield しない)。**アプリのバッファを IRQ から読むことはしない**。

**`pcm_close` = drain、期限つき** (往復 1 B10/B11、往復 2 R9、往復 3 B4): RUNNING で DRAINING に (OPEN でステージングに
データがあれば `pcm_start` してから)。advance がステージングを空にし、最後のデータの半分を通過し、さらに 1 半分 (0) を
通過したら STOP_REQ に入る。foreground は **STOP_DONE か FAULTED になるまで IF=1 で待つ** (期限 = staged frame ÷ rate +
3 半周期 + 3 tick を tick に切り上げ。44.1k で最小 15 tick、22.05k で 29 tick)。期限切れは番犬と同じく drain 失敗 → STOP_REQ
を待つ。STOP_DONE なら `irq_unregister` → `dma_pool_free` × 2 → CLOSED、戻り 0 (drain 失敗が記録されていれば `OS32_ERR_IO`)。
FAULTED なら leaked、`OS32_ERR_IO`。**「PI が来ない」は証拠にしない**。末尾の 0 埋めは underrun に数えない。

**RESYNC と停止のサンプル境界** (往復 3 B6): PEN=0 は発行済みの DMA 要求の最後のサンプル転送が終わってから効く (DS139PP2
p.16/19)。だから **PEN=0 の後、I11 の DRS (D4: PDRQ/CDRQ が動作中) が 0 になるのを tick で待ってから** `dma_chan_mask` する
(RS_STOP / STOP_REQ の表)。CS4231 (V=100) と CS4231A の I11 の並びは同じ (Appendix A に差分無し — **実装時に再確認**)。再始動
(RS_RESTART) は mask 状態でリングを組み直してから unmask → PEN=1 なので、サンプル内のバイト位置は先頭からそろう。

**所有と回収** (往復 1 B13、往復 2 R10、往復 3 非 blocker): owner は **既存の資源 owner の規則と同じアプリ ID**
(`appslot_cur()` / `snd_focus` が使う id。CPL=0 のシェルは APP_ID_SHELL = 1)。`pcm_write / status / close / set_volume` は owner
一致を要求 (不一致は `OS32_ERR_INVAL`)。回収は `exec_reclaim_owned(id)` に `pcm_reclaim(id)` を足す (呼ばれる時点では `g_cur_app`
は親に戻っているので、**保存した owner と引数 id だけを照合**): **待たない abort** — 状態が RUNNING / DRAINING / RS_* なら
IEN=0 → PEN=0 → ack → `dma_chan_mask` (DRS は待たない。捨てるストリームなので境界は問わない) → 証拠の読み戻し 1 回 →
`irq_unregister` → 証拠があれば `dma_pool_free` × 2、無ければ leaked/FAULTED。STOP_REQ / STOP_DONE の途中なら**残りの手順を
引き継いで 1 度だけ**解放する (`finalized` フラグ。close 側の待ちは longjmp で戻らないので二重解放は起きないが、契約として
1 度に固定する)。正常終了・fault・CTRL+STOP・park 中の kill で同じ経路。

**レート**: 44100 / 22050 だけ。**音量**: I6/I7 の LDA/RDA (6 ビット、1.5dB 刻み、減衰値に線形) を `percent` (1〜100、100 = 0dB)
から写し、**0 は D7 の LDM/RDM (ミュート) を立てる** (1〜100 で落とす)。101 以上は `OS32_ERR_INVAL`。

### 2-2. KAPI (v61、末尾追記。出力引数は出力保護つき、入力は `ring3_user_range_ok`)

```c
int  pcm_open(u32 rate);                          /* 0 / NOSYS / BUSY (他の owner が open 中) / INVAL (rate) / NOMEM / IO (初期化の期限) */
int  pcm_write(const void *buf, u32 bytes);       /* 受け取ったバイト数 (frame の倍数、0 = 満杯) / 負 = 未 open・非 owner・範囲外 */
int  pcm_status(u32 *free_bytes, u32 *counters);  /* free_bytes = ステージングの空き。counters = (underruns<<24) | (drain_failed<<16) | resyncs (8/8/16 ビット、255/255/65535 で飽和)。出力 2 本、1-5 と同じ保護 */
int  pcm_close(void);                             /* drain、期限つき。0 / 負 (abort に落ちた = OS32_ERR_IO) */
int  pcm_set_volume(u32 percent);
```

### 2-3. 純粋関数 (ホスト試験)

`pcm_advance` の判定 (pos → half、切り替え検出、underrun / late_fill の計数、RESYNC の条件)、`pcm_write` の受け入れ長 (frame 倍数・空き・半分の境界)、レート → I8 の値、percent → 減衰、初期化列と停止列を**ポート書きの列**として返して照合 (順序の変異: I14 → I15、MCE 無しの I9、MODE2 無しの I24)、状態機械の遷移表と不正遷移。

## 3. 受入

| ID | 見るもの | 手段 |
|---|---|---|
| E0 | **前提**: NP21/W の `/api/sound` に CS4231 を足す: PEN / IEN / MODE2 / I8 / Base / `bufdatas` / **PI の累積回数** (NP21/W の `totalsample` は周期内残量なので新設) / PU 回数 / DMA ch と IRQ / 0F40h の値、および **直近 1 秒の出力 PCM を base64 で返す `?pcm=1`**: 採取点は `cs4231g.c` の DMA 読み取りバッファ → ミックス前 (コーデックの rate、16 ビットステレオの生 frame、音量とホスト rate 変換の**前**、他音源が混ざる前)。**未 attach (`dmach = 0xff`) での `cs4231_control` の配列外アクセスの防御**も E0 で入れる。NP21/W は自前フォーク (`make build && make deploy` は NP21/W 停止が要る) | np21w-src |
| E1 | ホスト試験 (変異つき): 2-3 の全部 | `check-par` |
| E2 | NP21/W (**別 trial ini で `SNDboard=0x64` = [D2]**): 検出 (0F43h + I12 + I25) → 起動行 `[pcm] CS4231 v=101 irq 10 dma 1 fmt 0x5B` | NP21/W |
| E3 | CPL=3 の `pcm_test`: **左 = 1kHz 正弦、右 = frame 番号の下位 16 ビット** (左右取り違え・旧半分の反復・ミュートを見分ける) を 5 秒 → E0 の PCM (生 frame) を取って左右の内容を照合 (右の frame 番号は連続、左は 1kHz の周期 44.1 frame ± 1、許容: 欠落 0 frame)、PI 累積 ≒ 5 × 44100 / 2048、underruns 0、`pcm_status` の free (ステージングの空き) が書き込みで 4 バイト単位に減り**半分の補充で 8KB 戻る**。書き込みを 200ms 止めて無音 (underruns +、意図した停止区間は別に集計) → 再開後に右チャネルの番号が飛ばずに続く。ミュート (percent 0) は I6/I7 の D7 と音量適用後の出力で別に確認 | NP21/W |
| E4 | 共有 IRQ の実証 (証明範囲を明記): PCM だけ / 偽装置だけ / **同時 = 試験用 hook を実 PI の IRQ10 dispatch の入口 (PCM が I24 を ack する前) に置き、PI=1 を確認してから偽 pending を立て、同じ dispatch で両方の handler の結果を記録** / 2 巡目 = 偽 handler の初回走査の後に PCM handler が偽要因を立てる順序 / 偽装置の tick 回収 = IRQ を注入せず pending だけ立てる。偽装置は 2 つ目の物理要因ではない (電気的共有は実機の 82557 で) | NP21/W |
| E5 | CPL=3・CUI の `pcm_test` を CTRL+STOP で殺す → `pcm_reclaim` で PEN=0・DMA マスク・次の open が通る。GUI の park 中の kill と CPL=0 は対象外と明記 | NP21/W |
| E6 | 実機 (Ra266) — 独立した受入: MODE1 の初期状態、INIT 中の書き無視、MCE / 校正の待ち、Base 上位書きのロード、DAC の初期ミュート、**XTAL2 の有無** (44.1k が出るか)、BIOS の 0F40h 旧値、1MB 超 DMA、DRQ/FIFO の停止タイミング、auto-init ビット、実際の音 (耳) | 実機 |

## 4. しないこと

録音、PIO、8 ビット、モノラル (アプリ側で複製)、ミキサの入力側、86 型 PCM の FIFO の模倣 (P4)、`0C2Bh/0C2Dh` の再配置、DMA #0/#3、I8 の XTAL1 系レート (48k 等。XTAL2 が無い機械への対応は E6 の結果で決める)。

## 5. 決裁が要る点

1. NP21/W の trial ini の複製に `SNDboard=0x64` (PC-9801-86 + MATE-X PCM、既存の OPN も残る) を入れる ([D2]。原本は触らない)。
2. E0 の NP21/W フォーク拡張は NP21/W の停止 → 配備 → 起動を伴う ([D1] と同じ型)。
