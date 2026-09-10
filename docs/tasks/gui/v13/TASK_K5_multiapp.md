# K5: GUI アプリ 4 本の同時実行 (契約 T2a) — v1.3 の最初の票

> 発行: PM (2026-09-10) / レーン: K (C、カーネル背骨) / 前提: ユーザー決裁 B ([AUDIT §6](AUDIT_2026-09-10.md))
> 親: [PLAN.md](PLAN.md) / 契約: T2, T2a, T3, T4, U8 ([API_CONTRACTS](../API_CONTRACTS.md))、
> v1.2 S1 (暫定「1 本」を撤廃)、S2〜S8 ([v12 CONTRACTS](../v12/CONTRACTS.md))
> 排他: `kernel/**` `exec/**` `kapi/**` `include/memmap.h` `sdk/kapi.json` + 生成物。
> gshell 側 (`userland/gshell/**`) は K5b で W レーンと調停する。

## ゴール

凍結契約 T2/T2a のとおり、**GUI アプリを最大 4 本同時に生かす**。
PD 切替が起きるのは `gui_call(OP_WAIT)` の中だけ (T2a「譲り合いの点は OP_WAIT だけ」)。
5 本目は `ERR_FULL`。アプリ終了・kill 時はその**アプリだけ**の資源を回収する (T4/U8)。
CUI シェル (`shell.bin`) の入れ子 `exec_run` は変えない。

これは端末 (外部アプリ) と CUI 子の同居の土台であり、v1.3 の残件すべての前提になる。

### 「同時」の意味 (2026-09-10 ユーザー確認済み) — Win3.1 の GetMessage 方式

| やること | やらないこと |
|---|---|
| アプリ 4 本が**メモリに載ったまま**生きている (窓が残る) | タイマ割込みでの切替 (プリエンプション) |
| 動くのは常に **1 本だけ**。他は `OP_WAIT` の中で止まっている | 裏で別アプリが走る、スレッド、優先度、タイムスライス |
| 切替は、動いているアプリが `OP_WAIT` に入ったとき、WM が「イベントの届いた別のアプリ」を 1 本起こす (窓をクリックしたらそのアプリが次に動く) | 動いている最中の横取り |

本票で「切替」「起こす」と書くのはすべてこの意味。ROADMAP §2 の「timer interrupt を利用した
multi-task」(v2.0 で検討) は**本票の範囲外**で、その足場も作らない。

## 現状の事実 (2026-09-10 に PM が静的確認)

| # | 事実 | 根拠 |
|---|---|---|
| F1 | 生きている ring3 アドレス空間は 1 本 (`g_ring3_as`)。`exec_run` は**呼び出し側を塞ぐ** (setjmp/longjmp で子の終了まで戻らない) | `exec/exec.c:213`、`ExecContext.jmpbuf`、`run_program` (`gshell/src/lib.rs:233`) |
| F2 | ExecContext は**入れ子のスタック** (`exec_ctx_stack[MAX_EXEC_NEST=4]`)。段ごとに sbrk/heap/stack/guard を持ち、注記に「PD切り替え不要」 | `exec/exec.c:136-153`、`exec/exec.h:26` |
| F3 | 資源の所有者 = **ネスト段** (`cur_res_owner`)。FD / redirect / pipe / GUI 窓 / タイマの回収は `*_owned(exec_nest_level)` で段ごと | `fs/fd_redirect.c:21-22`、`exec/exec.c:531-540`、`kernel/gui.c:71` |
| F4 | アプリの物理ページは `pgalloc_mark_used(ca/cb)` で **0x500000 以上の固定物理帯**を使い、仮想=物理 (identity)。exec_heap/sbrk のアドレスも物理そのもの | `exec/exec.c:977-978,1064`、ExecContext 注記「アイデンティティマッピング」 |
| F5 | それでも PD はアプリごとに作れる (`paging_addrspace_create_n` + `map_user_range`)。同じ仮想 0x500000 に別物理を置く機構は**既にある** (共有ライブラリの .data/.bss がそれ) | `kernel/paging.h:191-237`、契約 T2a 可視性の注記 |
| F6 | カーネルスタックは 1 本。syscall は `int80_stub` → `ring3_syscall_dispatch(frame*)` で、`pushad` フレームがカーネルスタック上。`OP_WAIT` は gshell (CPL=0 常駐) の `op_wait` が `wm_cycle` + `sys_halt` で回す | `kernel/ring3_entry.asm:32-`、`gshell/src/handler.rs:355-` |
| F7 | gshell は既に 4 スロット (`GUI_SLOT_MAX`) と owner を持ち、`OP_INIT` は常に 0 を返す | `gshell/src/wm.rs:404`、契約 T2a |
| F8 | 起動失敗・fault・CTRL+STOP の畳み方は「1 本」前提: `ring3_fault_kill` は master CR3 復帰 → AS 破棄 → longjmp | `exec/exec.c:277-`、`exec.c:234-302` |

## 段階 K5a — 設計 (読取専用) + ホストモデル

コードを変えない。次を**根拠付きで**決めて本票の §設計 に書き、ホストで検証できる状態モデルを添える。

1. **物理ページとアドレス** (F4/F5): アプリごとに物理ページを `pgalloc` から取り、固定仮想 0x500000 へ写す。
   ローダ・`exec_heap`・sbrk・スタック・guard ページが identity を前提にしている箇所を全部列挙し、
   仮想≠物理でも成り立つ形を示す。共有ライブラリ帯 (0x400000〜) の .text 共有 / .data 別はそのまま。
2. **アプリごとのカーネル側コンテキスト** (F6): 切替はどこで、何を保存するか。候補は
   (a) アプリごとのカーネルスタック (4 × 16KB) を持ち `ESP`+`CR3`+`TSS.esp0` を入れ替える、
   (b) `OP_WAIT` の syscall フレームだけを保存して単一カーネルスタックで済ませる。
   切替を呼ぶのは gshell の `op_wait` (新 KAPI `gui_yield(next)` 等) か、カーネルの `gui_call` 出口か。
   IF の状態 (`int80_stub` の `sti`/`cli` 対) と、切替中の IRQ を明記する。
3. **所有者 ID** (F3): 所有者を「ネスト段」から「アプリ ID (1 = シェル、2〜5 = アプリ)」へ。
   `res_owner_get/set` の意味、切替時に誰が set するか、`*_owned(id)` の回収がアプリ単位で
   閉じること。CUI 側 (入れ子 exec_run) では従来どおり段 = ID になるよう互換を示す。
4. **起動・切替・終了** (F1/F8): いまの `exec_run` は呼び出し側 (gshell top-level) を子の終了まで
   塞ぐ。4 本を生かすには「起動したアプリが最初の `OP_WAIT` に入ったら WM へ戻る」形が要る
   (塞がない起動)。その名前・引数は設計で決める。切替は **GetMessage 方式**: `OP_WAIT` で
   止まっているアプリのうちイベントの届いたものを WM が 1 本起こすだけで、スケジューラは持たない。
   アプリの `exit` / fault / CTRL+STOP / 起動失敗のそれぞれで、その ID だけを畳んで WM へ戻る経路。
   フォーカスの無いアプリへの CTRL+STOP は届かない (T6 はフォーカス窓のアプリ宛)。
5. **メモリの勘定**: 4 本が要求どおりに入らないときは**拒否** (`ERR_NOMEM` / `ERR_FULL`)。スワップは
   しない (ユーザー方針 2026-09-10、[memory `APP_BAND_PDE`](../../memory/APP_BAND_PDE.md))。
   8MB (CUI) / 9MB / 15MB (PEGC) の各構成で、gshell + アプリ n 本の見積表を出す。
6. **SHM スロット** (F7): `OP_INIT` で 0〜3 を配り、5 本目は `ERR_FULL`。所有者 ID との対応。
7. **変えないもの**: `shell.bin` の入れ子 `exec_run`、v86、`sys_switch_shell` (T9)、
   `mkos32x --cpl0` の例外扱い。KAPI 追加は末尾追記のみ ([ABI2])、版数は PM が決める ([ABI3])。
8. **ホストモデル**: スケジューラ・所有者・スロットの状態遷移を `tools/tests/multiapp_model_host.c`
   (または Rust) に切り出し、`test_multiapp_model.py` で「5 本目拒否 / 終了で 1 本分だけ回収 /
   切替は OP_WAIT でだけ / fault で他アプリの資源に触らない」を RED→GREEN で示す。
   `make check` への登録は PM。

成果物: 本票 §設計 (追記)、ホストモデル + TDD 記録 (`tools/tests/multiapp_model_tdd.md`)、
KAPI 追加候補の一覧 (名前・引数・戻り値・エラー)。**実装はしない。** 独立レビュー (ユーザー経由) を
通してから PM が契約を凍結し、K5b を発注する。

## 段階 K5b — 実装 (K5a 凍結後に発注)

カーネル (K) と gshell (W) を分けて発注する。受入はゲスト実機:

| ID | 試験 | 合格条件 |
|---|---|---|
| G1 | `gui_demo` と `gui_bench` を同時に起動 | 両方の窓が出て、それぞれのクリックが自分に届く (`gui_bench` の `CLICK n`) |
| G2 | 片方を閉じる | もう片方は動き続け、閉じた側の FD / 窓 / タイマだけが回収される |
| G3 | 5 本目 | `ERR_FULL` で起動されず、既存 4 本は無事 |
| G4 | フォーカス窓のアプリに CTRL+STOP | そのアプリだけ畳まれる。他は無事 |
| G5 | fault するアプリ (`ring3_fault`) を 1 本混ぜる | そのアプリだけ kill、`fault_kill_count` +1、他は無事 |
| G6 | 8MB / 15MB 構成 | 見積表どおりの本数まで起動でき、超えたら拒否 |
| G7 | 切替点 | `OP_WAIT` 以外で PD が変わらない (カーネルのカウンタで証明) |
| G8 | CUI 回帰 | `shell.bin` の入れ子 exec、v86、regress 6 本が従来どおり |

## この票に含めないもの

端末アプリ本体、console の差し込み口、入力統合 (K6 以降)。設定レジストリ (S0)。
