# TASK_ATAPI_TIMEOUT — ATAPI の待ち上限を秒単位にする

> 状態: **票 (未着手)**。発行: PM (Claude Code `claude-opus-5-5`)、2026-09-26。
> 関係: `drivers/atapi.c` / `drivers/ide.h` (`IDE_TIMEOUT_LOOP`)、[`../../05_drivers.md`](../../05_drivers.md) §5-6、
> ATAPI 装置選びの修正 (wt/cd-fix、2026-09-26 着地) の代行レビュー (Fable 5.1) の P3。

## 0. 問題

ATAPI の待ち (BSY / DRQ の解除) の上限が **`IDE_TIMEOUT_LOOP` = 1,000,000 回の `inp`** で、時間ではなくバスの速さで決まる。
Ra266 では 0.5〜1 秒の桁。実機の CD ドライブは次の場面でこれより長く BSY のままになりうる:

| 場面 | 規定・実機での長さ | 今の扱い |
|---|---|---|
| 回転が止まった後の最初の READ(10) (スピンアップ) | 2〜4 秒 | 期限切れ → 1 セクタずつ → `atapi_recover` が**健全な装置のコマンドを DEVICE RESET で捨てる** → UA → 出し直し。読みが 1 回失敗して上の層へ返る |
| SRST の後 (`atapi_srst`) | ATA は最大 **31 秒**を許す | 2ms + 約 1 秒で諦め、戻り値も捨てている。遅い装置では BSY 中に DRV_HEAD を書く (2 回目のレビューの P3 が後ろへずれただけ) |

## 1. やること

1. 待ちの上限を **`tick_count` の秒単位**にする。割り込みが閉じている場面 (起動時の `atapi_init`、KAPI 経由の読み) では tick が進まないので、
   `cpu_delay_us` (校正済みの空回し、1 回 100ms まで) を塊で回す時間に切り替える (`atapi_delay_us` の流儀)。どちらを使うかを場面ごとに決める。
2. 上限: 通常の PACKET は 5〜10 秒、**`atapi_srst` の BSY 待ちは 31 秒** (他より長く、規定の最大)。`atapi_srst` の戻り値を呼び手で見る。
3. 「遅いだけ」と「固まった」の境目を秒で持ち、DEVICE RESET はその境目を越えたときだけにする。
4. ホスト試験の模型にスピンアップ (READ の後しばらく BSY) と、SRST 後の長い BSY を足す。

## 2. 同じ機会に見ること (実機で踏んだら)

- 媒体なしを **ASC 3Ah 以外**で返す装置 (例: 04h/02h「initializing command required」= START UNIT が要る) は、待っても変わらないのに
  `atapi_read_capacity` (mount / cdinst) のたびに 5 秒待って媒体なしになる。ASC/ASCQ を診断行に出し、04h/02h には START STOP UNIT を出す。
- 装置が 2 台あると `atapi_init` の待ちは最大 10 秒 (1 台 5 秒 × 2)。

## 3. 受け入れ

- T1: ホスト試験で、スピンアップ 3 秒の READ が 1 回目で読める (DEVICE RESET 0 回)。SRST 後 10 秒 BSY の装置を待ちきる。
- T2: 起動の最悪時間 (装置 2 台とも応答なし) を計算して票と docs/05_drivers.md に書く。
- T3: 実機 Ra266 で、CD を 5 分放置した後の `ls /cd0` が 1 回で通る (`atapi_get_stats` の dev_resets が増えない)。
