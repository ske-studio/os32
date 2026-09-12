# T8-K (full-screen GFX の画面所有者、カーネル + KAPI v48) — ホスト TDD の記録

票: [docs/tasks/gui/v13/TASK_T8_fullscreen_gfx.md](../../docs/tasks/gui/v13/TASK_T8_fullscreen_gfx.md) §2 の D1 / D1a / D3
実装: `exec/appslot.c` + `exec/appslot.h` (所有者の表と判定) / `exec/exec.c` (`hdr_flags` の控え・
`--cpl0` の GUI 拒否・回収の並び) / `gfx/gfx_core.c` + `gfx/gfx.h` (KAPI の門) /
`kernel/kselftest.c` / `sdk/kapi.json` (v48)
試験: `tools/tests/multiapp_impl_host.c` のケース 20 (`python3 -B tools/tests/test_multiapp_impl.py`)

## 様式

`k5b_kernel_tdd.md` / `k7_tdd.md` と同じで、**模型ではなく実物**を見る:

1. `multiapp_impl_host.c` が実物の `exec/appslot.c` を `#include` する (1 行も写さない)。
2. ホスト ILP32 GNU89 (`gcc -m32 -march=i386 -ffreestanding -Werror -nostdlib`) で走らせ、
   **同じソース**をカーネルと同じフラグの `i386-elf-gcc -Werror` でもコンパイルする ([C1])。
3. `make`・エミュレータ・配備は使わない。

### なぜ所有者の表が `exec/appslot.c` にあるのか

所有者そのものは GFX の持ち物に見えるが、**判定材料が 3 つとも AppSlot にある** —
走っている ID (`res_owner_get()`)、その ID が CPL=3 か (`cpl3`)、その ID の OS32X ヘッダ flags
(`hdr_flags`、T8 で追加)。GFX 側に置くと材料を取りに行く向きが逆になり、ホストで試験できない
(`gfx/gfx_core.c` は VRAM とポート I/O に触るので持ち込めない)。

そこで判定を**純関数** `appslot_gfx_claim_check(gui_mode, caller, cpl3, hdr_flags)` に切り出し、
状態を動かす `appslot_gfx_claim()` / `appslot_gfx_owner_exit()` と分けた。`gfx/gfx_core.c` に
残るのは KAPI の門 2 本 (`gfx_kapi_init` / `gfx_kapi_init_200`) と `gfx_screen_owner` だけで、
どれも 3 行以下。`sdk/kapi.json` の `"target"` がスロット `gfx_init` / `gfx_init_200` を門へ
向けているので、生成物は 1 行も手で触っていない ([ABI1])。

`exec/exec.c` はホストに持ち込めない (CR3 / setjmp / ページテーブル)。ケース 20 が実物を叩くのは
判定と回収の 2 つで、`exec_launch` が起動時に写す 2 語 (`ctx->cpl3` / `ctx->hdr_flags`) だけは
ハーネスの `ma_start_gfx()` に写してある — 写した部分の正しさは言わない。

## 試験の区分 — `multiapp_impl_host.c` ケース 20 (33 チェック)

| 検査 | 何を固定したか |
|---|---|
| 20a〜20c | 起動直後の所有者は WM (1)。アプリを立てただけでは移らない (`gfx_init` を呼んでいない) |
| 20d | **CUI 中 (con_sink 無効) は所有者を触らない** — gshell が居らず、全画面は従来どおり誰でも取れる |
| 20e〜20h | GUI 中の `gfx_init` で所有者がその ID へ移る。二度呼んでも変わらず、**他人の回収では戻らない** |
| 20i〜20l | 正常終了でも `exec_kill` でも所有者は WM へ戻る。park しただけでは戻らない (WM が上書きしない根拠) |
| 20m〜20q | 2 本目が取った画面は 2 本目のもの。画面を持っていない 1 本目の回収では所有者が動かない |
| 20r〜20u | **宣言 (`OS32X_FLAG_GFX`) の無い CPL=3 の `gfx_init` は `OS32_ERR_INVAL`**。所有者は動かず `gfx_init_reject_count` だけが 1 増える (D1a) |
| 20v〜20w | CUI 中は宣言が無くても従来どおり通り、素通しは拒否として数えない |
| 20x〜20z | CPL=0 の子 (`--cpl0`) は宣言があっても所有者にならない。WM 自身 (owner 1) の復帰の `gfx_init` も所有者を動かさない |
| 20A〜20D | **GUI からの `--cpl0` は生存アプリ 0 本でも `OS32_ERR_INVAL`** (D1)。拒否は池も帯も 1 つも動かさない |
| 20E〜20G | CUI (`exec_run`) 経路は従来どおり通り帯を claim する。シェル帯の載せ替えは GUI 判定の対象外 |

ケース 19 (K5b の `--cpl0` 受入) は `appslot_cpl0_admit` に `gui` 引数が増えたので、
CUI 経路 (`gui = 0`) を明示して同じ検査を通してある。ハーネスの `ma_start_cpl0_gui(is_shell, gui)`
は拒否の戻り値を `OS32_ERR_FULL` へ丸めずそのまま返すようにした (以前は丸めていたので、
`OS32_ERR_INVAL` と `OS32_ERR_FULL` を区別できなかった)。

## RED → GREEN

最初の RED はコンパイルエラー: `appslot_cpl0_admit` が 1 引数のままのハーネスが
「too few arguments to function」で落ちた (署名を広げた = K5b A1 の条件を広げた印)。

そこから、実装を 1 か所ずつ「ありそうな間違い」に差し替えて、試験が**その間違いだけ**を
捕まえることを見た。差し替えは全部戻してある (`exec/appslot.c` は GREEN の形)。

| # | 差し替え | 落ちた検査 |
|---|---|---|
| R1 | `appslot_gfx_claim` が所有者を取らない (`if (r > 0) g_gfx_owner = r;` を外す) | 20f / 20g / 20h / 20k / 20n |
| R2 | `appslot_gfx_owner_exit` を no-op にする (回収で戻さない) | 20j / 20l / 20o / 20q |
| R3 | 宣言ビットを見ない (`hdr_flags & OS32X_FLAG_GFX` の判定を外す) | 20s / 20t / 20u / 20v / 20w / 20x |
| R4 | `gui_mode` を見ない (CUI 中も所有者を取る) | 20d / 20v / 20w |
| R5 | `appslot_cpl0_admit` の `gui` 判定を外す (GUI からの `--cpl0` を通す) | 20A / 20B / 20C / 20D / 20E |

R3 で 20v / 20w (CUI 中は通る) と 20x (CPL=0 は取らない) も落ちるのは、判定を外すと
「本来なら CUI・CPL=0 の枝で素通しになるはずの呼び出しが所有者を取ってしまう」ため —
拒否と素通しが同じ関数の枝なので、片方を壊すともう片方も動く。

R4 で 20v / 20w が落ちるのも同じ理由 (CUI の素通しが GUI の判定へ落ちる)。

## GREEN の実測

```
$ python3 -B tools/tests/test_multiapp_impl.py
HOST ILP32 GNU89 COMPILE PASS
...
  ok   20G シェル帯の載せ替えは GUI 判定の対象外
ALL PASS
TARGET i386-elf GNU89 -Werror COMPILE PASS
```

221 チェック (K7 までの 188 + ケース 20 の 33)、失敗 0。

## kselftest (ブート時、実機)

`kernel/kselftest.c` の `test_gfx_owner()` が `appslot_gfx_owner_selftest()` を呼び、2 項を見る:

- `gfx owner moves on claim, returns on exit` — CUI 中は動かず、GUI 中に宣言付きで呼べば移り、
  他人の回収では戻らず、本人の回収で WM へ戻る
- `gfx claim without OS32X_FLAG_GFX is refused` — GUI 中の宣言なしは `OS32_ERR_INVAL` で
  所有者は動かず `gfx_init_reject_count` が 1 増え、CUI 中は通って数も増えない

借りるのは空きスロット `APP_ID_MAX` で、スロットの中身・`cur`・owner・所有者・カウンタは
丸ごと保存して戻す (`appslot_resume_mark_selftest` と同じ作法)。

**この項はまだ実機で踏んでいない** — `make` もエミュレータも使っていない ([V4])。
ホストでは `appslot_gfx_owner_selftest()` 自体は走らせていない (ケース 20 が同じ遷移を
より細かく見ているため)。

## 未実施

- `make` (clean ビルド / `make check` / `make external`) — コーダーの禁止事項。
  `tools/check_kapi_version.py` と `tools/check_constraints.py` は単体で走らせて通した。
  `tools/check_manifests.py` / `tools/check_privileged.py` はビルド成果物を要求するので未実行。
- 実機 (NP21/W) の受入 F1〜F7。配備・エミュレータ操作も禁止。
