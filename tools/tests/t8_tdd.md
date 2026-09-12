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

## 巻き込み — `boot_splash_native_host.c` (2026-09-12、f19dd00 の後)

`make check` の `check-boot-splash-host` が `undefined reference to 'con_sink_is_enabled'` /
`appslot_gfx_claim` / `appslot_gfx_owner` で落ちた。このハーネスは `gfx/gfx_core.c` を
**ホストで直リンク**するので、T8 で門 (`gfx_kapi_init` / `gfx_kapi_init_200` /
`gfx_screen_owner`) が増えた分だけ未定義参照が増える。

直したのは試験側だけ (`tools/tests/boot_splash_native_host.c` にスタブ 3 本)。このハーネスが
見るのはバックエンドの選択と 9801 のライフサイクルで、画面の所有者は対象外 — 所有者は
ケース 20 が実物の `exec/appslot.c` で見る。なので「CUI 中 / 誰も所有していない」=
門が素通しになる値 (`0` / `0` / `1`) を返すだけにした。**カーネル本体は 1 行も変えていない。**

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

---

# T8-2 (CUI 専用の宣言 + 「拒否は畳む」、カーネル + ビルド系) — 追記 2026-09-12

票: [TASK_T8_fullscreen_gfx.md](../../docs/tasks/gui/v13/TASK_T8_fullscreen_gfx.md) §6 の F5 / F1 後半
実装: `sdk/include/os32/os32_kapi_shared.h` (`OS32X_FLAG_CUI_ONLY` = 0x0010) / `sdk/mkos32x.py`
(`--cui-only`) / `build/programs.mk` + `build/app.conf` (4 列目 `gfx`|`cui`) /
`tools/check_manifests.py` §2b / `exec/appslot.c` + `exec/appslot.h` (`appslot_cui_only_admit`、
拒否で `abort_req`) / `exec/exec.c` (`exec_launch` の砦) / `gfx/gfx_core.c` (門の文言) /
`kernel/v86.c` (`v86_gui_refuse`) / `kernel/kselftest.c`
試験: `multiapp_impl_host.c` ケース 21 (`python3 -B tools/tests/test_multiapp_impl.py`)

## なぜ 2 つ足したのか

**F5 の不合格**: T8 は「GUI から V86 / VDM を起動させない」を `OS32X_FLAG_FORCE_CPL0` で
判定したが、`userland/cmds/v86.bin` の flags は **0x0**。v86 は CPL=3 のプログラムで、V86 へは
KAPI (`v86_selftest` / `v86_disktest` / `v86_boot` / `v86_boot2`) を通してカーネル側から入る。
CPL で判定する砦では原理的に捕まらないので、**宣言ビットを 1 つ増やした**。

**F6 の実測**: 宣言の無い `gfx_init` を「断って続行させる」と、プログラムは失敗を知らないまま
描画 KAPI と VRAM 直書きで描き続け、`gfx_init_reject_count` が 2 になった後に FPS 計測の絵が
上 200 ラインへ出た。**拒否 = そのアプリを畳む**に変えた。走っている本人なので `exec_kill` は
使えず、K5c の CTRL+STOP と同じく `abort_req` を立てて syscall 出口の `ring3_abort_check()` に
畳ませる (専用カウンタは増やさず `gfx_init_reject_count` / `v86_gui_reject_count` のまま)。
`ring3_abort_count` と `fault_kill_count` は畳む経路が同じなので一緒に増える (F3 と同じ註)。

## 試験の区分 — `multiapp_impl_host.c` ケース 21 (18 チェック)

| 検査 | 何を固定したか |
|---|---|
| 21a〜21c | 純関数 `appslot_cui_only_admit`: GUI からの宣言付きだけ `OS32_ERR_INVAL`。CUI は通り、宣言なしは GUI でも通る |
| 21d | 他のビット (`OS32X_FLAG_GFX`) と混ざっていても宣言を見る |
| 21e | **`FORCE_CPL0` では捕まらない** — F5 の不合格そのもの (v86.bin の flags は 0x0) |
| 21f〜21h | `exec_start` 経路 (池の admit → ヘッダ読み → cui_only の admit の順) で起動しない。拒否は池も枚数も 1 つも動かさない |
| 21i〜21j | CUI (`exec_run`) 経路は従来どおり通り、1 本立つ |
| 21k〜21o | 宣言の無い `gfx_init` の拒否で **`abort_req` が立つ** (= syscall 出口で畳まれる)。数は `gfx_init_reject_count` のまま |
| 21p〜21r | CUI 中の素通しは畳まない。二度目の拒否でも要求は立つ |

## RED → GREEN

| # | 差し替え | 落ちた検査 |
|---|---|---|
| R1 | `appslot_cui_only_admit` が `gui` を見ない (`(void)gui;` にして常に断る) | 21b / 21i / 21j |
| R2 | `appslot_cui_only_admit` が常に 0 を返す (宣言を見ない) | 21a / 21d / 21f / 21g / 21h / 21i / 21j |
| R3 | 拒否で `appslot_abort_request()` を呼ばない (断るだけ = T8 のまま) | 21n / 21q |
| R4 | 畳む位置を間違える (`claim` の判定の前に立てて素通しも畳む) | 21p |

R2 で 21i / 21j も落ちるのは、`ma_start_cui_only(1)` が通ってしまい GUI 側で 1 本立ち、
続く CUI の起動が池と `appslot_start_admit` の S2 判定に掛かるため (拒否と素通しが同じ枝)。

`--cui-only` の焼き込みは `sdk/mkos32x.py` を単体で実行して確かめた
(`--api 38 --cui-only` → ヘッダ offset 0x0C が `0x0010`)。
`tools/check_manifests.py` の §2b は関数を直接呼んで RED を 2 通り確かめた
(app.conf から `cui` を外す → `v86_* KAPI を呼ぶのに 'cui' が無い`、
4 列目を `cuix` にする → 書式 NG)。

## GREEN の実測

```
$ python3 -B tools/tests/test_multiapp_impl.py
HOST ILP32 GNU89 COMPILE PASS
...
  ok   21r 畳んだ後は 1 本も残らない
ALL PASS
TARGET i386-elf GNU89 -Werror COMPILE PASS
```

239 チェック (ケース 20 までの 221 + ケース 21 の 18)、失敗 0。
巻き込みで `tools/tests/boot_splash_native_host.c` に `shell_print` のスタブを 1 本足した
(門が拒否の理由を端末へ出すようになったため。カーネル本体は変えていない)。
`test_boot_splash_native.py` / `test_con_sink.py` / `test_multiapp_model.py` /
`test_kbd_inject.py` / `test_owner_reclaim.py` / `tools/check_constraints.py` も通した。

## kselftest

`test_gfx_owner()` に 1 項追加 —
`OS32X_FLAG_CUI_ONLY refused only from GUI` (`appslot_gfx_owner_selftest()` のビット 2)。
ビット 1 の側にも「拒否したアプリに `abort_req` が立つ / 素通しでは立たない」を足した。
**実機ではまだ踏んでいない** ([V4])。

## 未実施

- `make` (clean ビルド / `make check` / `make external`) — コーダーの禁止事項。
  `tools/check_constraints.py` と `tools/check_manifests.py` の §2b は単体で走らせて通した
  (§1 / §2 / §3 はビルド成果物を要求するので未実行)。
- 実機 (NP21/W) の受入 F5 / F6 再試験。配備・エミュレータ操作も禁止。
- gshell / 端末側の入口判定 (`classify` に `cui` を足す) は別票 T8-2W。
