# K6C — console の差し込み口 (GUI モード中のカーネル出力を端末モデルへ)

状態: **発行 (2026-09-12、PM)**。決裁 B の順序 (K5 → **K6 console** → K7 入力統合 → 端末アプリ) の 2 段目。
親: [PLAN.md](PLAN.md) §1。前提: K5b (GUI アプリ 4 本、`759d5bb` で main)。
ユーザー決裁: **端末は外部アプリ** (2026-09-10)。gshell は端末を持たない。

## 0. 目的

GUI モード中、カーネルと CUI コマンドが `kernel/console.c` の入口 (`shell_print*` / `console_write` /
カーソル系) に書く出力は、いまテキスト VRAM に描かれて (テキスト面は非表示) 消える。これを
**カーネル内のリング (シンク) に溜め、端末アプリが KAPI で吸って Paint する**ための差し込み口を作る。
この票は **K 側 (カーネル + KAPI v46) だけ**。端末アプリ側 (`userland/rust/t5a_display` が吸って描く) は
K6C-A として K 着地後に別票。K7 (入力統合: `kbd_getchar` ← GUI イベント、FEP) は含めない。

## 1. 接点 (確認済み)

- 入口は `kernel/console.c` の 6 本に集約されている: `shell_putchar` / `shell_print` (214) / `shell_print_dec` /
  `shell_print_hex32` / `shell_print_utf8` (250) / `console_write` (322)。`kprintf` もここへ落ちる。
  呼び出し元は 23 ファイル 128 か所あるが、差し込みは入口だけでよい。
- 既に `rshell_active` ならシリアルへ複写し、`v86_is_active()` なら描画を抑止している (= 「描画先を分ける」前例)。
- GUI モードの印: `console_text_gdc_stop()` / `console_text_gdc_start()` (437 行付近、gshell が GUI 入退場で呼ぶ)。
- 状態を変える入口: カーソル (`console_set_cursor`)、クリア (`cls` 相当の経路を洗い出すこと)、スクロールは
  リング側では「行の追加」に畳む。
- KAPI 版番号: **v46** (KAPI_SPEC §3-2 に「K6C console シンク」で予約してから実装。v43 はネットワーク予約)。

## 2. 設計 (PM 案、ここで決めた 3 点は変えない)

1. **リング容量 8KB、カーネル帯の静的配列** (kmalloc しない)。`MEMORY_BUDGET.md` に 8KB を計上する。
   あふれは **古い方を捨てる** (レコード単位)。捨てた回数をカウンタ `con_sink_drop_count` (カーネルシンボル、KAPI にしない) に積む。
2. **割込み文脈からの書き込みも通す**。push/pop は `cli`/`sti` (既存の IF 退避の作法) で守る。
   単一プロデューサを仮定しない。`v86_is_active()` 中も溜める (シリアル同様「出力は失わない」)。
3. **CUI へ戻るとき (`console_text_gdc_start`) はリングを捨てる** (テキスト VRAM が再び正になる)。
   GUI へ入るとき (`console_text_gdc_stop`) に空で始める。有効化のために別 KAPI は足さない — 既存の入退場に結び付ける。

レコード形式 (端末アプリが復元できる最小):

| type | payload | 発生源 |
|---|---|---|
| `PRINT` | `color u8`, `len u8`, UTF-8 バイト列 (最長 200、超えたら分割) | `shell_print*` / `console_write` / `kprintf` |
| `CLEAR` | なし | 画面クリアの経路 |
| `CURSOR` | `x u8`, `y u8` | `console_set_cursor` |

改行 / CR は `PRINT` のバイトとして流す (端末モデルが解釈する)。スクロールはレコードにしない。

KAPI v46 (末尾追記、owner 制限は下記):

| 名前 | 引数 | 戻り | 備考 |
|---|---|---|---|
| `con_sink_read` | `void *buf, u32 cap` | 書いたバイト数 (0 = 空)、負 = エラー | レコード境界で切る (途中で切らない)。**読み手は 1 本だけ** — 最初に読んだアプリ ID が所有し、`exec_exit` / `exec_kill` の owner 回収で解放 |
| `con_sink_stat` | `u32 *pending, u32 *dropped` | 0 | 溜まっているバイト数と捨てた回数 |

CPL=3 からのポインタは既存のディスパッチャ検証 (アプリ帯 / SHM) に通す。

## 3. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| C1 | CUI モード無影響 | regress 6 本、v86 -t、rshell が従来どおり。リングは触られない (`pending` 0) |
| C2 | GUI 中の出力が溜まる | gshell に入り、CUI コマンド (例: `ver`) を **将来の端末経路で**流す代わりに、K 側単体では **kselftest にケースを足して**リングの push/pop/あふれ/CUI 復帰時の破棄を確かめる (ゲストの kselftest_pass が +N) |
| C3 | KAPI | `v12_api_test` 相当の小さな CPL=3 試験で `con_sink_stat` が読めること、2 本目の読み手が拒否されること |
| C4 | 回収 | 読み手アプリを ESC / CTRL+STOP で畳んだ後、別アプリが読み手になれる |

## 4. 禁止 / 範囲外

- gshell に端末の描画を持たせない (T5b は撤去済み、契約 v13: WM はクライアント面を持たない)。
- `kbd` / 入力側は触らない (K7)。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。ホスト TDD は必須。
