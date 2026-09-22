# TASK_PCM_CS4231 — CS4231 (MATE-X PCM) の PCM 再生ドライバ (§5-5 の P1)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 v1 (レビュー前)**。
> 正典の関係: [`PLAN.md`](PLAN.md) §5-5 (P1 の行と「設計の前提」の表)、土台は [`TASK_HAL_WIRING.md`](TASK_HAL_WIRING.md) (1-1 割り込み、1-2 8237、1-3 プール、1-5 時計)。
> **実機不要**: NP21/W が CS4231 を再現する (`np21w-src/src/cbus/cs4231io.c`、`src/sound/cs4231c.c`)。実機 (Ra266 の MATE-X PCM) は後で同じコードを通す。

## 0. 範囲

**P1 だけ**: 16 ビット・ステレオ・44.1kHz (と 22.05kHz) の**再生**を、カーネル所有の DMA リングと KAPI で提供する。
アプリは「次のブロックを書く」だけ。録音・ミキサ (出力音量以外)・V86 への提供 (P4)・ソフト合成器 (P2) は含まない。
OS32 の `snd` エンジン (YM2203 のレジスタ書き) との接続は P3。

## 1. ハードウェアの事実 (`docs/hw/undocumented/io_sound.md`「PC-9821X･N内蔵型」、NP21/W の実装で照合)

| 項目 | 事実 |
|---|---|
| 判別 | `A460h` bit7-4 のサウンド ID (0111b = X･N 内蔵、MATE-X PCM)。`0F43h` の WSS ID (下位 6 ビット = 000100b) |
| ポート | `0F44h` Index Address / `0F45h` Indexed Data / `0F46h` Status / `0F47h` PIO Data (0C2Bh/0C2Dh で再配置可、既定のまま使う) |
| 割り込みと DMA | **`0F40h`**: bit5-3 = INT (001 INT0 = **IRQ3** / 010 INT2 = **IRQ6** / 011 INT41 = **IRQ10** / 100 INT5 = IRQ12)、bit2-0 = DMA (001 #0 / 010 #1 / 011 #3、111 = 使わない)。NP21/W: `cs4231irq[] = {ff,03,06,0a,0c,…}`、`cs4231dma[] = {ff,00,01,03,…}` で一致 |
| **選ぶ値** | **INT41 (IRQ10) + DMA #1** → `0F40h = 0x1A`。IRQ10 は 1-1 の動的な線 (3/5/6/8/9/10/14/15) で、LGY-98 の既定 (INT0 = 3 / INT1 = 5) と衝突しない。IRQ12 は固定スタブ (V86 用) なので使わない。DMA #1 は FDC (#2) と衝突しない (#3 は予備) |
| 8237 側 | 1-2 の `dma_chan_setup(1, ring_phys, 16384, DMA_DIR_FROM_MEM, DMA_MODE_CYCLIC)` (auto-init)。**リングは 64KB を跨がない** (1-3 のプールが保証)。NP21/W は auto-init の折り返し (`lastaddr → startaddr`) を模擬する |
| PI 割り込み | Playback Base (index 14/15) に「サンプル数 − 1」を置くと、その数ごとに PI (index 24 の bit4) が立ち、pin control (index 10) の IEN で INT が出る。NP21/W は `playcount` ごとに `pic_setirq(dmairq)` |
| レジスタ | WSS/CS4231 の索引レジスタ: 0 = 左入力 / 6,7 = 左右 DAC 出力減衰 / **8 = 再生フォーマット (bit7 16 ビット、bit4 ステレオ、bit3-0 クロック分周)** / **9 = インターフェイス (bit0 PEN 再生許可、bit6 PPIO は PIO なので 0)** / **10 = ピン制御 (bit1 IEN)** / 11 = エラー・初期化 (bit7 ACI、初期化中は Index 読みで INIT が立つ) / 12 = モード/ID (bit6 MODE2) / **14,15 = Playback Base 上位/下位** / **24 = 代替機能ステータス (bit4 PI、書いて消す)**。**ビットの並びはデータシート (CS4231A) で実装時に確認**して定数の注釈に典拠を書く |
| 初期化列 | `0F44h` に MCE (bit6) を立てて 8/9 を書く → MCE を落とす → ACI (index 11 bit5) が落ちるまで待つ (上限は tick で) → 10 の IEN → 9 の PEN。順序を間違えると鳴らない (NP21/W は寛容なので実機で出る型) |

## 2. 設計

### 2-1. カーネル `drivers/pcm_cs4231.c` (+ `pcm_cs4231_math.c` 純粋部)

- **検出**: `A460h` の ID と `0F43h` の WSS ID の両方が合うときだけ有効。無ければ KAPI は `OS32_ERR_NOSYS`。
- **リング**: 1-3 の `dma_pool_alloc(16384, 4096)` = **16KB = 4096 サンプル (16 ビット×2ch) = 半バッファ 2048 サンプル × 2** (§5-5 の前提、46ms × 2)。
  Playback Base = 2047 → **半バッファごとに PI**。driver は「いま装置が読んでいる半分」を PI で切り替え、空いた半分にアプリの
  データを写す。書き込み位置の確認に 1-2 の `dma_chan_remaining` を使う (診断とアンダーラン判定の補助。停止判定には使わない、1-2 の契約)。
- **割り込み** (1-1 の契約): `irq_register(10, pcm_irq, 0, IRQ_F_SHARED)`。handler は index 24 を読んで PI が無ければ `IRQ_NONE`、
  あれば PI を消して (書き込み) 半バッファを切り替え `IRQ_HANDLED`。**tick フック `pcm_tick()`** を `timer_handler` に足す
  (全登録者の定期回収の契約): PI を取りこぼしても remaining の位置から半分の切り替えを追いつかせ、アンダーラン (アプリが
  半分を埋めなかった) を数える。埋まらなかった半分は**無音 (0) で埋める** (ノイズより無音)。
- **状態機械**: CLOSED → OPEN (フォーマット設定、DMA はマスク) → RUNNING (PEN + unmask) → DRAINING (最後のブロックを流し切る) → CLOSED。
  `pcm_close` は PEN を落とす → `dma_chan_mask(1)` → 装置側の確認 (index 9 の PEN が 0、PI が来ない) → `irq_unregister` →
  `dma_pool_free`。停止を確認できなければ `dma_pool_mark_leaked` (1-3 の契約)。
- **所有**: open したアプリが owner。`exec_exit()` の所有者別回収に `pcm_owner_exit(owner)` を足す (`snd_owner_exit` と同じ型)。
  1 本だけ (同時 open は `OS32_ERR_BUSY`)。CPL=0 の常駐シェルも同じ口。
- **レート**: 44100 / 22050 だけ (CS4231 の分周表から選ぶ。8 の bit3-0)。それ以外は `OS32_ERR_INVAL`。フォーマットは 16 ビット
  リトルエンディアン・ステレオ固定 (P2 の合成器の出力に合わせる。モノラルはアプリ側で複製)。
- **音量**: index 6/7 の減衰を `pcm_set_volume(0..100)` で。既定は 0dB。

### 2-2. KAPI (v61、末尾追記。出力引数は TASK_HAL_WIRING 1-5 / TASK_KAPI_OUTPUT_GUARD の保護つき)

```c
int  pcm_open(u32 rate);                          /* 0 / NOSYS (無い) / BUSY (使用中) / INVAL (rate) */
int  pcm_write(const void *buf, u32 bytes);       /* 受け取ったバイト数 (0 = 満杯、負 = 未 open)。4 の倍数で切る */
int  pcm_status(u32 *free_bytes, u32 *underruns); /* 出力 2 本 (保護つき) */
int  pcm_close(void);
int  pcm_set_volume(u32 percent);
```

`pcm_write` は**コピー**する (アプリのバッファへ直接 DMA はしない — §5-5 と realhw/PLAN §1 の決定)。空きが半バッファ未満なら
入るぶんだけ受けて残りを返し、アプリは `sys_yield` して再試行する (協調型)。

### 2-3. 純粋関数 (ホスト試験)

半バッファの切り替え表 (PI と remaining からの位置判定、取りこぼし時の追いつき)、`pcm_write` の受け入れ長 (空き・4 の倍数・
半バッファ境界)、レート → 分周値、状態機械の遷移と不正遷移の拒否、初期化列の順序 (ポート書きの列を配列で返して照合)。

## 3. 受入

| ID | 見るもの | 手段 |
|---|---|---|
| E0 | **前提**: NP21/W の `/api/sound` に CS4231 の状態 (PEN・レート・フォーマット・`bufdatas`・PI 回数・DMA チャネル/IRQ) を足す (NP21/W は自前フォーク、`make build && make deploy` は NP21/W 停止が要る) | np21w-src |
| E1 | ホスト試験 (変異つき): 2-3 の全部 | `check-par` |
| E2 | NP21/W (**ini の `SNDboard=0x64` (PC-9801-86 + MATE-X PCM) が要る = [D2]**): 検出 → `0F40h` = 0x1A → `lspci` 相当の起動行 `[pcm] CS4231 irq 10 dma 1` |
| E3 | CPL=3 の試験 `pcm_test`: 1kHz 正弦波 5 秒 → `/api/sound` の PI 回数 ≒ 5 × 44100 / 2048、アンダーラン 0、`pcm_status` の free が半バッファ単位で動く。書き込みを止めて無音 (アンダーラン計上) → 再開 | NP21/W |
| E4 | 共有 IRQ の実証: 別の偽装置 (kselftest の型) を IRQ10 に同時登録し、PCM の PI と偽装置の要因を混ぜて両方が処理される (TASK_HAL_WIRING W3 の「実 IRQ の共有」を初めて実線で踏む) | NP21/W |
| E5 | exec_exit の回収: `pcm_test` を CTRL+STOP で殺す → PEN が落ち、DMA がマスクされ、次の open が通る | NP21/W |
| E6 | 実機 (Ra266): E2/E3/E5 + 実際の音 (ユーザーの耳) | 実機 |

## 4. しないこと

録音、PIO モード、8 ビット、モノラル、ミキサの入力側、86 型 PCM の FIFO の模倣 (P4)、`0C2Bh/0C2Dh` の再配置、DMA #0/#3 の使用。

## 5. 決裁が要る点

1. NP21/W の trial ini に `SNDboard=0x64` を入れた**別の trial ini** を作ってよいか ([D2]。原本は触らない。PCM を鳴らすには
   MATE-X PCM が要り、既存の OPN も残す)。
2. IRQ10 + DMA #1 の選択 (上の表の理由)。実機で BIOS が別の値を入れていれば `0F40h` を上書きする。
