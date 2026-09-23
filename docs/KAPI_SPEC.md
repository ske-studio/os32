# KernelAPI v62 仕様書

外部プログラム (OS32X) がカーネル機能を利用するためのAPIテーブル仕様。

---

## §1 概要

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| KAPIテーブルアドレス | 動的算出 (KHEAP_BASE + KHEAP_SIZE) |
| KAPIマジック | 0x4B415049 ('KAPI') |
| プログラムロード先 | 0x400000 |
| 最大プログラムサイズ | 1MB |
| プログラム専用ヒープ | 動的配置 (sbrk_heap_limit, exec_heap 管理下) |
| プログラム専用スタック | 動的配置 (メモリ終端付近、下向き展開) |
| 現在のバージョン | **52** |
| 合計エントリ数 | **219** (ヘッダ2 + 関数ポインタ215 + データフィールド2) |

---

## §2 呼び出し規約

| 対象 | コンパイルフラグ | 規約 |
|------|--------|------|
| カーネル本体 | `gcc -m32` | System V i386 ABI (スタック渡し) |
| KernelAPIラッパー | `__attribute__((cdecl))` または通常 | cdecl/System V |
| 外部プログラム | `gcc -m32 -ffreestanding` | System V i386 ABI |

外部プログラムの `main` は `void main(int argc, char **argv, KernelAPI *api)` のシグネチャを持ちます。crt0 が argc/argv と共に api ポインタを渡します。

---

## §3 ビルド手順

```bash
# 一括ビルド (Makefile利用)
make programs
```
外部プログラムは `userland/` 以下 (標準アプリなら `apps/`) に `.c` を置き、`make programs` / `make apps` を実行することで、`crt0.asm` や `libos32` (newlib-nanoラッパー) とともにリンクされ、`mkos32x.py` によってヘッダが付与された `.bin` が生成されます。

### §3-1 KernelAPI 関数の追加手順

**`sdk/kapi.json` が唯一の情報源 (SSOT)。** 構造体・ラッパー・初期化コード・Rust
バインディングはすべてここから生成されるので、生成物を直接編集してはならない。

> この節が手順の正典。CLAUDE.md / DEVELOPMENT.md / POLICY_DEV.md と
> スキル `.claude/skills/os32-kapi-add` はここを指すだけで、手順本文を持たない。

1. `sdk/kapi.json` の `api` 配列の**末尾**にエントリを追加する。
   既存スロットの並べ替え・削除は**禁止** (ビルド済みバイナリの ABI が壊れる)。
   必要なヘッダは同ファイルの `includes` に、プロトタイプは `externs` に追加する。
   - `target` — 実体の関数名がエントリ名と異なる場合に指定
   - `body` — ラッパー本体をインラインで書く場合に指定
   - `out` — **非 const のポインタ引数があるときは必須** (書式は §3-3)。
     書き忘れは `make check` の `check-kapi-out` が落とす
2. `sdk/kapi.json` の `"version"` と `sdk/include/os32/os32_kapi_shared.h` の
   `KAPI_VERSION` を**両方**インクリメントする (一致必須)。
3. 再生成して差分が意図した追加のみであることを確認する:
   ```bash
   python3 sdk/gen_kapi.py && python3 sdk/kapi_rust_gen.py
   git diff --stat
   ```
   生成対象: `sdk/include/os32/os32_kapi_generated.h` / `sdk/include/os32/os32_kapi_slots.h`
   (`KAPI_SLOT_<NAME>` = `int 0x80` のスロット番号。型に依存しないので crt0 非依存の
   自己完結テストからも include できる) / `kapi/kapi_generated.c` /
   `exec/exec_kapi_init.inc` / `sdk/rust/os32api/src/kapi_generated.rs`。
   GitHub Actions (`.github/workflows/check.yml`) が生成物と kapi.json の一致を検査する。
4. カーネル側に実体を実装する。
5. 本仕様書のオフセット表を更新し、新APIに依存するプログラムの
   `build/app.conf` の要求バージョンを引き上げる。

値を持つフィールド (関数ポインタでないもの) は `data_fields` に追加する。
ジェネレータは `kapi-><field> = 0;` を出力するだけなので、実際の値は
`exec/exec.c` の `exec_init()` (または `exec_run()`) で代入すること。

### §3-2 版番号の予約 (未実装の追加を先取りで調停)

KAPI は append-only で版番号は単調増加。複数の計画が独立に「次版」を想定すると
版番号だけが衝突する (関数スロットは末尾追記なので実体は衝突しない)。実装着手時に
迷わないよう、**予約を下表で一元管理する。ここが版番号割り当ての正典。**

| 版 | 状態 | 追加内容 | 出典 |
|---|---|---|---|
| v40 | **実装済み** | GUI HAL 枠 `gfx_screen_info` / `gfx_hw_fill_rect` / `gfx_hw_blit` ほか | 本書 §4 |
| v42 | **実装済み (2026-09-06)** | GUI v1.1: `gui_call` / `gui_register` / `gfx_stats` / `gfx_lease_palette` / `sys_switch_shell` / `kbd_dropped_count` / `kbd_trygetrawkey` (レビュー ⑥) / `ime_feed_key` / `ime_set_render` (W2)。開発中は v41 と呼んでいたが、ime_* の追記を機に main マージ前に **v42 として確定** (2026-09-06 ユーザー承諾)。v41 の成果物は存在しない (main 未リリース) | [archive/gui_v11/TASK_K1](archive/gui_v11/TASK_K1_gui_call.md) |
| v43 | **欠番** (Host Services に予約していたが v44〜v50 が先に実装された。使わない) | — | [archive/network/TASK_N0.md](archive/network/TASK_N0.md) §1 |
| v44 | **実装済み (2026-09-11、K5b-K)** | GUI v1.3 K5: アプリ 4 本の同時実行 (契約 T2a、GetMessage 方式) `exec_start` / `exec_resume` / `exec_park` / `exec_kill` / `exec_app_state` と、フォーカス追従の音の排他 `snd_focus`。owner 1 (シェル帯) 専用。カウンタはカーネルシンボル (KAPI にしない)。v43 はネットワークに予約済みなので**飛ばした** | [tasks/gui/v13/TASK_K5_multiapp.md §D8](tasks/gui/v13/TASK_K5_multiapp.md)、[TASK_K5B_kernel](tasks/gui/v13/TASK_K5B_kernel.md) |
| v45 | **実装済み (2026-09-11、K5c)** | GUI v1.3 K5: `exec_abort_clear` — CTRL+STOP の宛先を**フォーカス窓のアプリ**にする (契約 T6、決裁 A1)。IRQ1 は走っているアプリにしか要求を立てられないので、WM が本人の要求を降ろしてからフォーカス窓の ID を `exec_kill` する。owner 1 (シェル帯) 専用 | [tasks/gui/v13/TASK_K5B_gshell.md §決裁](tasks/gui/v13/TASK_K5B_gshell.md) |
| v46 | **実装済み (2026-09-12、K6C)** | GUI v1.3 K6C: console シンク `con_sink_read` / `con_sink_stat` — GUI モード中のカーネル出力をリングに溜め、端末アプリ (外部) が吸う。読み手は 1 本 (owner 回収)。同じ追記で `sys_ram_kb` (K6-RAM 決裁 (2): 起動時に登録した実 RAM の合計 KB) も足した | [tasks/gui/v13/TASK_K6C_console.md](tasks/gui/v13/TASK_K6C_console.md)、[TASK_K6_ram_ceiling.md](tasks/gui/v13/TASK_K6_ram_ceiling.md) |
| v47 | **実装済み (2026-09-12、K7-K)** | GUI v1.3 K7 入力統合: `kbd_inject` (con_sink の読み手専用、UTF-8 を 256B の注入リングへ) / `kbd_inject_pending` (未読バイト数、誰でも可)。GUI 中の `kbd_getchar` / `kbd_getkey` は第 2 の park 点 (`APP_STATE_WAIT_KEY`) になり、`exec_resume` が注入リングから 1 バイトを EAX に入れる。同じ追記でエラー番号 -14 `OS32_ERR_AGAIN` を取った | [tasks/gui/v13/TASK_K7_input.md](tasks/gui/v13/TASK_K7_input.md) |
| v48 | **実装済み (2026-09-12、T8-K)** | GUI v1.3 T8 full-screen GFX 復帰: `gfx_screen_owner` (画面の所有者 = `gfx_init` / `gfx_init_200` を呼んだ CPL=3 アプリ、回収で WM へ戻る)。同じ追記で「GUI 中に `OS32X_FLAG_GFX` の無い CPL=3 の `gfx_init` を断る」(D1a) と「`--cpl0` は GUI から起動させない」(D1) を入れた。WM の present を捨てる D2 は**落とした** (2026-09-12 ユーザー決裁) | [tasks/gui/v13/TASK_T8_fullscreen_gfx.md](tasks/gui/v13/TASK_T8_fullscreen_gfx.md) |
| v49 | **実装済み (2026-09-12、T9-K)** | GUI v1.3 T9 shell script: 起動要求表 8 本 — `launch_req` / `launch_pending` / `launch_take` / `launch_report` / `launch_poll` / `launch_cancel` / `launch_child` と `sys_yield`。GUI 中の CPL=3 は入れ子 `exec_run` を使えないので、外部プログラムの起動と kill をカーネルの表に載せ owner 1 (WM) が仲介する。同じ追記で `exec_kill` を「id と子孫を末尾から回収」に固定した (D8) | [tasks/gui/v13/TASK_T9_sh.md](tasks/gui/v13/TASK_T9_sh.md) |
| v50 | **実装済み (2026-09-13、S0-K)** | 設定レジストリ: `db_open_existing` (RO / RW、CREATE 無し) / `db_prepare_only` / `db_bind_int` / `db_bind_text` / `db_bind_blob` / `db_bind_null` / `db_error_code` の 7 本 (slot 201〜207、data_fields は 0x348 / 0x34C へ)。既存 `db_*` 10 本は不変 | [tasks/settings/TASK_S0.md §1a](tasks/settings/TASK_S0.md) |
| v51 | **実装済み (2026-09-14、N1)** | ネットワーク Host Services `host_open` / `host_status` / `host_read` / `host_write` / `host_close` の 5 本 (slot 208〜212 = 0x348〜0x358、data_fields は 0x35C / 0x360 へ)。非ブロッキング (プロトコルを進めるのは 100Hz の `link_tick` だけ)、同時 2 ハンドル、ストリーム 1 本。実体は `kapi/kapi_host.c` + `net/link.c` | [archive/network/TASK_N0.md](archive/network/TASK_N0.md) §1a |
| v52 | **実装済み (2026-09-15、H3)** | 更新日時の保存 `sys_set_mtime` 1 本 (slot 213 = 0x35C、data_fields は 0x360 / 0x364 へ)。`VfsOps` の**任意実装フック** `set_mtime` を通し、**ext2 のみ実装**。持たない FS は `OS32_ERR_NOSYS` (失敗ではなく「持っていない」)。実体は `kapi/kapi_sys.c` + `fs/vfs.c` + `fs/ext2_vfs.c` | [tasks/shell/TASK_H3.md](tasks/shell/TASK_H3.md) |
| v53 | **実装済み (2026-09-16、H2)** | 排他的作成 `KAPI_O_EXCL` (`0x0400`)。**スロットは 1 本も増えていない** — `sys_open` のフラグが 1 つ増え、その**意味が広がった**ので版数を上げた ([ABI3])。`VfsOps` の**任意実装フック** `create_excl` を通し、**ext2 のみ実装**。持たない FS は `OS32_ERR_NOSYS`。実体は `fs/vfs_fd.c` + `fs/ext2_vfs.c`。同じ票で ext2 の置き換え rename の順序も変えた (宛先の名前を消さない) | [tasks/shell/TASK_H2.md](tasks/shell/TASK_H2.md) |
| v54 | **実装済み (2026-09-16、継承バグ)** | 覗くだけのキー取得 `kbd_peekkey` 1 本 (slot 214 = 0x360、data_fields は 0x364 / 0x368 へ)。キューを**1 バイトも動かさず**に 次のキーを返す (無ければ -1)。戻り値の形は `kbd_trygetkey` と同じ。`script_exec` の毎行の ESC 監視が `kbd_trygetkey` で打鍵を**取り出して捨てて**いたのを直す。実体は `drivers/kbd.c` (+ `drivers/serial.c` の `serial_peekchar` と `kernel/kbd_inject.c` の `kbd_inject_peek`) | [tasks/shell/INHERITED_BUGS.md](tasks/shell/INHERITED_BUGS.md) |
| v55 | **実装済み (2026-09-16、$?)** | 終了コードの配線 `exec_last_result` 1 本 (slot 215 = 0x364、data_fields は 0x368 / 0x36C へ)。直前の `exec_run` の結果を**種別 + 値**で返す。種別 (`EXEC_KIND_*`) は畳んだ側が渡すので `exit(-2)` / fault / CTRL+STOP を値ではなく種別で見分けられる。`exec_run` は**すべての return 点で**記録を書くので、起動しなかった場合に前回の記録が残らない。GUI 経路 (`exec_start` / `exec_resume`) の子は記録しない。実体は `exec/exec.c` | [tasks/shell/TASK_EXIT_STATUS.md](tasks/shell/TASK_EXIT_STATUS.md) |
| v56 | **実装済み (2026-09-22、実機シリアル)** | V･FAST モード `serial_init_vfast` / `serial_get_status` の 2 本 (slot 216 = 0x368、217 = 0x36C、data_fields は 0x370 / 0x374 へ)。FIFO 搭載機 (`0136h` bit6 の反転で判定) で `013Ah` bit7 を立てて **8253 と無関係に** 115200bps まで出す。**起動時の既定 9600 は互換モードのまま** — V･FAST は `serial N` で明示的に入る。併せて `serial_putchar` の TxRDY 待ちを「10ms tick まで寝る」から「1 文字時間 × 2 の予算で `cpu_delay_us` を挟んで見る」へ。実体は `drivers/serial.c` / `drivers/serial_plan.c` | [tasks/realhw/TASK_SERIAL_VFAST.md](tasks/realhw/TASK_SERIAL_VFAST.md) |
| v57 | **実装済み (2026-09-22、実機シリアル往復 4)** | ローカル打鍵だけの読み口 `kbd_trygetchar_local` 1 本 (slot 218 = 0x370、data_fields は 0x374 / 0x378 へ)。cooked リングだけを見て**シリアルも注入リングも見ない**。rshell の速度切替の番犬が「この 1 バイトはシリアル由来か」を**1 回の読みで**確定できるようにする (2 度読みの窓を消す)。⚠ 番号は PM が着地時に振り直す (同日に L-A も v57 を取得) | [tasks/realhw/TASK_SERIAL_VFAST.md](tasks/realhw/TASK_SERIAL_VFAST.md) |
| v58 | **実装済み (2026-09-22、手元ビルドのみ)** | 実機の PCI 列挙 `pci_count` / `pci_get` / `pci_cfg_read32` の 3 本 (slot 219 = 0x374、220 = 0x378、221 = 0x37C、data_fields は 0x380 / 0x384 へ)。起動時に `pci_init()` が コンフィギュレーションメカニズム #1 (`0CF8h` DWORD / `0CFCh`) で bus 0 を走査し、vendor/device/class/BAR/Interrupt Line を静的表 (上限 32) に記録する。**読むだけ** — BAR のサイズ判定 (全 1 を書いて読み戻す) はしないので BIOS の割り当てを壊さない。シェルの `lspci` / `pcidump` がこの 3 本を使う。**NP21/W は PCI を実装していない**ので、エミュレータでは `[pci] mech#1 absent` と `lspci: no PCI` が正しい姿。実体は `drivers/pci.c` / `drivers/pci_decode.c` | [tasks/realhw/TASK_LAN_82557.md](tasks/realhw/TASK_LAN_82557.md) |
| v59 | **実装済み (2026-09-23、手元ビルドのみ)** | µs 時計 `sys_time_now` 1 本 (slot 222 = 0x380、data_fields は 0x384 / 0x388 へ)。起動からの経過を µs で返す。**64 ビットは KAPI で返せない** (往復 1 の B14) ので、出力引数 2 本に**同じスナップショットの上下**を書く。時間源は `tick_count` (§1-0 の後はどちらのシステムクロックでもちょうど 10ms) と PIT ch0 のラッチ読みで、周期の境界はPIC1 の IRR bit0 をラッチの前後で挟んで判定する (最大 3 回やり直す)。戻り 0 = 成功 / `OS32_ERR_AGAIN` = 3 回とも判定できなかった / `OS32_ERR_NOSYS` = PIT 未初期化か mode 2 でない / `OS32_ERR_INVAL` = `lo` か `hi` が NULL・4 バイトが帯境界を跨ぐ・**2 本の範囲が交差する (差 0〜3)**。**負のときは 2 本とも書かない**。出力が読み取り専用の USER ページ (共有ライブラリの `.text`) なら `ring3_fault_kill` — OS32 は CR0.WP = 0 なのでハードウェアは止めない。実体は `kernel/ktime.c` / `kernel/time_math.c`、検証は `kapi/kapi_sys.c` の `kapi_sys_time_now` | [tasks/v3/TASK_HAL_WIRING.md](tasks/v3/TASK_HAL_WIRING.md) §1-5 |
| v60 | **実装済み (2026-09-23、手元ビルドのみ)** | PCI 結線の診断の取得口 `pci_bind_info` 1 本 (slot 223 = 0x384、data_fields は 0x388 / 0x38C へ)。`idx` 番目 (**`pci_get` と同じ列挙順**) の結線結果を呼び手のバッファへ**8 バイトちょうど**写す。並びは `drivers/pci_bind.h` の `struct pci_bind_info`。`result` = NONE / BOUND / DECLINED / QUARANTINED、`reason` は上書き規則 1 つだけが正、`line_state` は**読む時点で合成**する (結線のときは正常だった線が後から隔離されても `result` は BOUND のまま `line_state` だけが QUARANTINED になる)。**既存 `pci_get` の 40 バイトは広げない** — 旧呼び手のバッファを踏むので別の口にした。戻り 0 = 成功 / `OS32_ERR_INVAL` = `out` が NULL・8 バイトが帯境界を跨ぐ・`idx` が範囲外 (**負のときは 1 バイトも書かない**)。出力が読み取り専用の USER ページなら `ring3_fault_kill` (v59 と同じ規則)。シェルの `lspci` が注記を出す。実体は `drivers/pci_bind.c`、検証は `kapi/kapi_sys.c` の `kapi_pci_bind_info` | [tasks/v3/TASK_HAL_WIRING.md](tasks/v3/TASK_HAL_WIRING.md) §1-4 |
| v61 | **実装済み (2026-09-23、手元ビルドのみ)** | CS4231 (MATE-X PCM) の再生 `pcm_open` / `pcm_write` / `pcm_status` / `pcm_close` / `pcm_set_volume` の 5 本 (slot 224〜228 = 0x388〜0x398、data_fields は 0x39C / 0x3A0 へ)。16 ビット・ステレオ・44.1k / 22.05kHz の**再生だけ**で、単位は frame (左右 1 組 = 4 バイト)。カーネルが DMA リング 16KB (`dma_pool`) とステージング 16KB (`kmalloc`) を持ち、**アプリのバッファを IRQ から読むことはしない** — `pcm_write` はステージングへ写すだけで、リングを書くのは `pcm_advance` (IRQ / tick、IF=0) と停止中の `pcm_start` / RS_RESTART に限る。所有者は既存の資源 owner と同じアプリ ID で、異常終了は `exec_reclaim_owned` の `pcm_reclaim` が**待たずに**止めて返す。実体は `drivers/pcm_cs4231.c` / `drivers/pcm_cs4231_math.c` | [tasks/v3/TASK_PCM_CS4231.md](tasks/v3/TASK_PCM_CS4231.md) |
| v62 | **実装済み (2026-09-23、手元ビルドのみ)** | キーボード 8251 の診断 `kbd_diag` 1 本 (slot 229 = 0x39C、data_fields は 0x3A0 / 0x3A4 へ)。`KbdDiag` (24 バイト、`os32_kapi_shared.h`) を呼び手のバッファへ写す — IRQ1 回数・空 IRQ (RxRDY = 0)・エラー (PE/FE)・オーバーラン (OE だけ、バイトは使う)・起動時に読み捨てたバイト数・`kbd_init` の前後の 0043h・直近の 0043h とスキャンコード・書いたコマンド語・呼んだ時点の 0043h。戻り 0 / `OS32_ERR_INVAL` (`out` が NULL)。出力は生成ラッパの `out` 検査 (読み取り専用の USER ページなら `ring3_fault_kill`)。シェルの `kbdstat` が 1 行で出す。同じ変更でカーネルが 0043h に書くコマンド語を **0x14 → 0x16** (DTR = 1 = RTY# HIGH、BIOS の定常値) に直した — 実機 PC-9821Ra266 で打鍵が一切届かなかった件。実体は `drivers/kbd.c` / `drivers/kbd_status.c` | [POLICY_DEBUG.md](POLICY_DEBUG.md) §4-57 |

調停 (2026-09-06、同日改訂): GUI (K1〜W2) を先に実装するので **v42 = GUI、v43 = ネットワーク Host Services**
に確定。実装順が入れ替わるときは、着手前にこの表を更新してから版番号を取ること。
先に実装した側が実際の版番号を確定し、後発は次版へずらす。

**エラー番号の予約** (`os32_kapi_shared.h`、現在 -1〜-10): GUI (K1) が **-11 `STALE` /
-12 `VERSION` / -13 `FULL`**、GUI v1.3 K7 (v47) が **-14 `AGAIN`** (「いまは無い / 後で
もう一度」— 注入リングが空のときの `exec_resume`) を取る。ネットワーク Host Services は
既存の -1〜-10 に写像し (送信失敗→`IO` -1、引数不正→`INVAL` -9、未対応→`NOSYS` -10 等)、
固有の番号が要るときだけ **-18 以降**を使う (GUI の -11〜-14 と FS の -15〜-17 を避ける)。
ネットワーク側はまだ 1 つも番号を使っていないので、K7 が -14 を取り開始点を 1 つ下げた
(2026-09-12)。票 B8 往復 5 (2026-09-15、ユーザー決裁 2) が **-15 `ROFS`** (「書き込みを
受け付けない」— ext2 がメタデータの I/O エラーでエラー状態に入った後の書き込み系操作) を取り、
開始点をさらに 1 つ下げた。**番号の追加だけで構造体・スロットは変えないので KAPI 版数は据え置き。**
票 TASK_VFS_FD_PATH (2026-09-24) が **-16 `NAMETOOLONG`** (パスが長すぎる / 深すぎる — VFS は
切り詰めずに断る) と **-17 `BUSY`** (使用中 — 開いている SQLite DB・ジャーナル・その祖先の rename、
使用中の loop イメージの unlink / 置き換え) を取り、ネットワークの開始点を **-18 以降**へ下げた。
同じ票で既存の **-11 `STALE`** を VFS の FD にも使う (unlink・置き換え rename・umount で失効した FD の
read / write / fstat / seek)。版数は据え置き。

### §3-3 出力ポインタの宣言 `out` (票 TASK_KAPI_OUTPUT_GUARD)

OS32 は **CR0.WP = 0** で走る (`kernel/shlib.c` がカーネルからの書き込みで共有ライブラリを
張るため。[02_memory.md §2-1](02_memory.md) のページングの節)。そのため CPL=0 の KAPI ラッパは、
CPL=3 が出力引数に渡した**読み取り専用の USER ページ** — 共有ライブラリの `.text` / `.rodata`、
全アプリで同じ物理 — にも #PF を起こさずに書ける。ディスパッチャの早期検査
(`kapi_argptr` → `ring3_ptr_ok`) は「帯の中か」しか見ないので素通りする。

そこで **各エントリは出力ポインタを `out` で申告する**。`sdk/gen_kapi.py` が
`kapi/kapi_generated.c` のラッパ先頭 (target を呼ぶ**前**) に検査を出す:

| 書式 | 意味 |
|---|---|
| `"out": [{"arg": "buf", "len": "size"}]` | `size` バイトの出力バッファ |
| `"out": [{"arg": "buf", "len": "count", "unit": 512}]` | `count` × `unit` バイト (`unit` は整数か C の式) |
| `"out": [{"arg": "info", "size": "sizeof(IdeInfo)"}]` | 固定長 (整数か C の式。式は kapi.json の `includes` で見える型のみ) |
| `"out": "none"` | 非 const のポインタが**入力**か関数ポインタ (`mem_free` / `sys_shm_lock` / `sys_shm_free` / `sys_ls` / `gui_register` / `gfx_present_raster` / `ime_set_render`) |
| `"out": "target"` | target / body 自身が `ring3_user_ranges_writable` で検査している (`sys_time_now` / `pci_bind_info`)。ここで二重に見ると CR3 の往復が 2 倍になる |

1 本の KAPI に出力が複数あるときは**全部を並べる**。生成されるコードは
**全範囲を検査してから target を呼ぶ**ので、2 つ目が不可のときに 1 つ目だけ書かれることはない。

検査の中身 (`exec/exec.c` の `ring3_user_ranges_writable`): IF=0 → master CR3 へ切り替え →
アプリ PD の PDE/PTE が範囲の全ページで present + RW + USER か → CR3 復元 → IF 復元。
**CPL=0 の直呼び (常駐シェル / gshell) は素通し** (`ring3_in_syscall` で判定)。
不合格は `ring3_fault_kill()` = アプリの死 (戻らない)。長さの扱いは

- **NULL はその範囲を見ない** — KAPI ごとの既存の NULL の扱い (無視 / 負を返す / 死ぬ) を変えない
- **長さ 0 / 負 も見ない** (`int` の長さ引数は 0 に丸める)
- 個数 × 単位があふれたら `0xFFFFFFFF` = 必ず拒否

既知の穴: `dev_blk_read` の `unit` は**最小のセクタ長 512** で見る。1024 (FD) / 2048 (CD) の
デバイスでは後ろ半分以上が未検査のまま (帯検査は効く)。`dev->sect_size` はラッパからは引けない。

---

## §4 KernelAPI 構造体レイアウト

### ヘッダ

| Offset | フィールド | 説明 |
|--------|-----------|------|
| 0x00 | magic | 0x4B415049 ("KAPI") |
| 0x04 | version | APIバージョン (現在: 41) |

### API関数 (自動生成 — os32_kapi_generated.h 準拠)

> **`sys_getcwd` (0x7C) が返すポインタの寿命** (票 T9 §12 R1、2026-09-13):
> CPL=3 の呼び手には **KAPI トランポリンページ内の写し** が返る (カーネル帯の
> `fs/vfs.c` の static `cwd` は USER ビットが無く、CPL=3 が読むと #PF → fault
> kill になるため)。写しは **呼ばれるたびに同じ 1 本を上書き**するので、
> 呼び手は **次の KAPI 呼び出しより前に読み切る** こと (保持したいなら自分の
> バッファへ写す)。CPL=0 の呼び手 (常駐シェル) には従来どおり static `cwd` が
> そのまま返る。スロット・引数・戻り型は不変で、KAPI の版も上げていない
> (`sdk/kapi.json` の target を `vfs_cwd` → `vfs_cwd_user` に差し替えただけ)。

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x08 | gfx_init | `void(void)` |
| 0x0C | gfx_init_200 | `void(void)` |
| 0x10 | gfx_shutdown | `void(void)` |
| 0x14 | gfx_present | `void(void)` |
| 0x18 | kbd_trygetchar | `int(void)` |
| 0x1C | mem_alloc | `void *(u32 size)` |
| 0x20 | mem_free | `void(void *ptr)` |
| 0x24 | get_tick | `u32(void)` |
| 0x28 | kprintf | `void(u8 attr, const char *fmt, ...)` |
| 0x2C | sys_unlink | `int(const char *path)` |
| 0x30 | sys_rename | `int(const char *oldpath, const char *newpath)` |
| 0x34 | sys_mkdir | `int(const char *path)` |
| 0x38 | sys_ls | `int(const char *path, void *cb, void *ctx)` |
| 0x3C | kmalloc_total | `u32(void)` |
| 0x40 | kmalloc_used | `u32(void)` |
| 0x44 | kmalloc_free | `u32(void)` |
| 0x48 | paging_enabled | `int(void)` |
| 0x4C | rtc_read | `void(void *rtc_time)` |
| 0x50 | tvram_clear | `void(void)` |
| 0x54 | tvram_putchar_at | `void(int x, int y, char ch, u8 attr)` |
| 0x58 | tvram_putkanji_at | `void(int x, int y, u16 jis, u8 attr)` |
| 0x5C | tvram_scroll | `void(void)` |
| 0x60 | kbd_getchar | `int(void)` |
| 0x64 | kbd_getkey | `int(void)` |
| 0x68 | kbd_trygetkey | `int(void)` |
| 0x6C | sys_mount | `int(const char *prefix, const char *dev, const char *fs)` |
| 0x70 | sys_umount | `void(const char *prefix)` |
| 0x74 | sys_is_mounted | `int(const char *prefix)` |
| 0x78 | sys_chdir | `int(const char *path)` |
| 0x7C | sys_getcwd | `const char *(void)` |
| 0x80 | vfs_devname | `const char *(const char *prefix)` |
| 0x84 | vfs_sync | `int(void)` |
| 0x88 | sys_rmdir | `int(const char *path)` |
| 0x8C | serial_init | `void(u32 baud)` |
| 0x90 | serial_puts | `void(const char *s)` |
| 0x94 | serial_getchar | `int(void)` |
| 0x98 | serial_putchar | `int(u8 ch)` |
| 0x9C | serial_trygetchar | `int(void)` |
| 0xA0 | serial_is_initialized | `int(void)` |
| 0xA4 | exec_run | `int(const char *path)` |
| 0xA8 | dev_count | `int(void)` |
| 0xAC | dev_get_info | `int(int idx, char *name, int nm, int *type, u32 *sects)` |
| 0xB0 | fm_startup_sound | `void(void)` |
| 0xB4 | fm_play_mml | `void(const char *mml)` |
| 0xB8 | np2_detect | `int(void)` |
| 0xBC | np2_get_version | `void(char *buf, int size)` |
| 0xC0 | np2_get_cpu | `void(char *buf, int size)` |
| 0xC4 | np2_get_clock | `void(char *buf, int size)` |
| 0xC8 | np2_check_hostdrv | `int(char *buf, int size)` |
| 0xCC | ide_init | `void(void)` |
| 0xD0 | ide_drive_present | `int(int drv)` |
| 0xD4 | ide_identify | `int(int drv, void *info)` |
| 0xD8 | ide_read_sector | `int(int drv, u32 lba, void *buf)` |
| 0xDC | path_get_drive | `const char *(void)` |
| 0xE0 | path_get_cwd | `const char *(void)` |
| 0xE4 | path_set_drive | `int(const char *d)` |
| 0xE8 | path_set_cwd | `void(const char *p)` |
| 0xEC | path_parse | `void(const char *input, void *result)` |
| 0xF0 | ext2_format | `int(int drv, u32 sectors)` |
| 0xF4 | kcg_init | `void(void)` |
| 0xF8 | kcg_set_scale | `void(int s)` |
| 0xFC | buz_on | `void(void)` |
| 0x100 | buz_off | `void(void)` |
| 0x104 | rshell_set_active | `void(int active)` |
| 0x108 | ide_write_sector | `int(int drv, u32 lba, const void *buf)` |
| 0x10C | ide_write_sectors | `int(int drv, u32 lba, u32 cnt, const void *buf)` |
| 0x110 | sys_reboot | `void(void)` |
| 0x114 | sys_halt | `void(void)` |
| 0x118 | shell_putchar | `void(char ch, u8 attr)` |
| 0x11C | shell_print_utf8 | `void(const char *utf8_str, u8 color)` |
| 0x120 | console_get_cursor_x | `int(void)` |
| 0x124 | console_get_cursor_y | `int(void)` |
| 0x128 | console_set_cursor | `void(int x, int y)` |
| 0x12C | sys_open | `int(const char *path, int mode)` |
| 0x130 | sys_close | `void(int fd)` |
| 0x134 | sys_read | `int(int fd, void *buf, u32 size)` |
| 0x138 | sys_write | `int(int fd, const void *buf, u32 size)` |
| 0x13C | sys_lseek | `int(int fd, int offset, int whence)` |
| 0x140 | console_get_size | `void(int *w, int *h)` |
| 0x144 | kbd_get_modifiers | `u32(void)` |
| 0x148 | sys_get_mem_kb | `u32(void)` |
| 0x14C | sys_time | `os_time_t(void)` |
| 0x150 | gfx_hardware_scroll | `void(int lines)` |
| 0x154 | gfx_present_rect | `void(int x, int y, int w, int h)` |
| 0x158 | sys_exit | `void(int status)` |
| 0x15C | sys_isatty | `int(int fd)` |
| 0x160 | sys_stat | `int(const char *path, OS32_Stat *buf)` |
| 0x164 | sys_fstat | `int(int fd, OS32_Stat *buf)` |
| 0x168 | gfx_set_palette | `void(int idx, u8 r, u8 g, u8 b)` |
| 0x16C | gfx_get_palette | `void(int idx, u8 *r, u8 *g, u8 *b)` |
| 0x170 | gfx_get_framebuffer | `void(void *fb)` |
| 0x174 | gfx_add_dirty_rect | `void(int x, int y, int w, int h)` |
| 0x178 | gfx_present_dirty | `void(void)` |
| 0x17C | gfx_present_nosync | `void(void)` |
| 0x180 | gfx_present_raster | `void(void *table)` |
| 0x184 | kcg_read_ank | `void(u8 ch, u8 *buf)` |
| 0x188 | kcg_read_kanji | `void(u16 jis_code, u8 *buf)` |
| 0x18C | sys_shm_alloc | `void *(int blocks)` |
| 0x190 | sys_shm_lock | `int(void *ptr)` |
| 0x194 | sys_shm_free | `int(void *ptr)` |
| 0x198 | ime_getchar | `int(void)` |
| 0x19C | ime_trygetchar | `int(void)` |
| 0x1A0 | ime_toggle | `void(void)` |
| 0x1A4 | ime_is_active | `int(void)` |
| 0x1A8 | ime_set_mode | `void(int mode)` |
| 0x1AC | ime_get_mode | `int(void)` |
| 0x1B0 | ime_getkey | `int(void)` |
| 0x1B4 | sys_redirect_fd | `int(int fd, const char *path, int mode)` |
| 0x1B8 | sys_reset_redirect | `void(int fd)` |
| 0x1BC | sys_is_redirected | `int(int fd)` |
| 0x1C0 | sys_pipe_alloc | `int(void)` |
| 0x1C4 | sys_pipe_free | `void(int id)` |
| 0x1C8 | sys_pipe_get_buf | `u8 *(int id)` |
| 0x1CC | sys_pipe_get_len | `u32(int id)` |
| 0x1D0 | sys_pipe_clear | `void(int id)` |
| 0x1D4 | sys_redirect_fd_buf | `int(int fd, u8 *buf, u32 size, u32 len)` |
| 0x1D8 | sys_redirect_get_buf_len | `u32(int fd)` |
| 0x1DC | paging_is_present | `int(u32 addr)` |
| 0x1E0 | snd_bgm_play | `void(const char *mml)` |
| 0x1E4 | snd_bgm_stop | `void(void)` |
| 0x1E8 | snd_bgm_is_playing | `int(void)` |
| 0x1EC | snd_se_play | `void(int se_id)` |
| 0x1F0 | snd_se_play_raw | `void(int note, int duration_ticks, int tone)` |
| 0x1F4 | snd_set_master | `void(int enable)` |
| 0x1F8 | snd_bgm_set_persist | `void(int persist)` |
| 0x1FC | kbd_is_pressed | `int(int scancode)` |
| 0x200 | fm_note_on | `void(int ch, int note)` |
| 0x204 | fm_note_off | `void(int ch)` |
| 0x208 | fm_set_tone_num | `void(int ch, int tone_num)` |
| 0x20C | ssg_tone | `void(int ch, u16 period)` |
| 0x210 | ssg_volume | `void(int ch, u8 vol)` |
| 0x214 | ssg_all_off | `void(void)` |
| 0x218 | mouse_poll | `void(void *info)` |
| 0x21C | mouse_available | `int(void)` |
| 0x220 | mouse_set_bounds | `void(i16 x_min, i16 y_min, i16 x_max, i16 y_max)` |
| 0x224 | tvram_readchar_at | `void(int x, int y, u16 *code, u8 *attr)` |
| 0x228 | tvram_reverse_cell | `int(int x, int y)` |
| 0x22C | mouse_cursor_set_mode | `void(int mode)` |
| 0x230 | mouse_cursor_show | `void(void)` |
| 0x234 | mouse_cursor_hide | `void(void)` |

### DB API (v29-v31)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x238 | db_open | `int(const char *path)` |
| 0x23C | db_close | `int(int handle)` |
| 0x240 | db_exec | `int(int handle, const char *sql)` |
| 0x244 | db_prepare | `int(int handle, const char *sql)` |
| 0x248 | db_step | `int(int handle)` |
| 0x24C | db_column_int | `int(int handle, int col)` |
| 0x250 | db_column_text | `const char *(int handle, int col)` |
| 0x254 | db_finalize | `int(int handle)` |
| 0x258 | db_last_error | `const char *(int handle)` |
| 0x25C | db_mem_used | `u32(void)` |

### フォント (v32)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x260 | kcg_load_font | `int(const char *path)` |

### デバイス / ループバック / システム情報 (v33)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x264 | ide_get_info | `int(int drv, void *info)` |
| 0x268 | sys_get_build_info | `void(char *buf, int size)` |
| 0x26C | loop_attach | `int(const char *path, int slot)` |
| 0x270 | loop_detach | `void(int slot)` |
| 0x274 | loop_status | `int(int slot, u32 *total, int *bps)` |
| 0x278 | dev_blk_read | `int(const char *dev_name, u32 lba, int count, void *buf)` |
| 0x27C | dev_blk_write | `int(const char *dev_name, u32 lba, int count, const void *buf)` |

### FEP 辞書管理 / ノンブロッキング入力 (v35)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x280 | ime_switch_dict | `int(int variant)` |
| 0x284 | ime_user_list | `int(const char *yomi_prefix, void *out, int max)` |
| 0x288 | ime_user_delete | `int(const char *yomi, const char *kanji)` |
| 0x28C | ime_user_export | `int(const char *path)` |
| 0x290 | ime_user_clear | `int(void)` |
| 0x294 | ime_trygetkey | `int(void)` |

### V86 / VDM (v36-v39)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x298 | v86_selftest | `int(void)` |
| 0x29C | v86_disktest | `int(const char *path)` |
| 0x2A0 | v86_boot | `int(const char *path)` |
| 0x2A4 | v86_boot2 | `int(const char *path, const char *second)` |

`v86_boot` は HDD イメージ (NHD/HDI) から、`v86_boot2` は 2 イメージ構成で
V86 ゲストを起動する。MS-DOS 5.00A の起動確認に使う。

### GUI HAL 枠 (v40)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x2A8 | gfx_screen_info | `void(void *out)` |
| 0x2AC | gfx_hw_fill_rect | `int(int x, int y, int w, int h, u8 color)` |
| 0x2B0 | gfx_hw_blit | `int(int dx, int dy, int sx, int sy, int w, int h)` |
| 0x2B4 | gui_call | `int(u32 op, u32 arg)` |
| 0x2B8 | gui_register | `int(void *handler, void *pump)` |
| 0x2BC | gfx_stats | `int(void *out)` |
| 0x2C0 | gfx_lease_palette | `int(int first, int count, const u8 *rgb)` |
| 0x2C4 | sys_switch_shell | `int(const char *path)` |
| 0x2C8 | kbd_dropped_count | `u32(void)` |
| 0x2CC | kbd_trygetrawkey | `int(void)` |
| 0x2D0 | ime_feed_key | `int(int keydata)` |
| 0x2D4 | ime_set_render | `void(void *table)` |

### アプリ 4 本の同時実行 + 音の排他 (v44)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x2D8 | exec_start | `i32(const char *cmdline)` |
| 0x2DC | exec_resume | `i32(i32 app_id, i32 wait_ret)` |
| 0x2E0 | exec_park | `i32(void)` |
| 0x2E4 | exec_kill | `i32(i32 app_id)` |
| 0x2E8 | exec_app_state | `i32(i32 app_id)` |
| 0x2EC | snd_focus | `i32(int app_id)` |

いずれも **owner 1 (シェル帯 = gshell / CUI シェル) からのみ**。判定は `gui_register` と同じ形で、
それ以外からは `OS32_ERR_INVAL`。設計の正典は
[tasks/gui/v13/TASK_K5_multiapp.md](tasks/gui/v13/TASK_K5_multiapp.md) の D0〜D11。

- `exec_start(cmdline)`: **塞がない起動**。>0 = app_id (2〜5) で最初の `gui_call(OP_WAIT)` まで
  進んで park した / 0 = park より前に終了した (回収済み・`gui_owner_exit` 配送済み) /
  <0 = 起動しなかった (`OS32_ERR_FULL` = ID の池が尽きた、`EXEC_ERR_NOMEM` = 物理が足りない、
  `EXEC_ERR_NOT_FOUND`、`EXEC_ERR_INVALID`)。従来の `exec_run` は残り、CUI の入れ子はそのまま。
- `exec_resume(app_id, wait_ret)`: park してあるアプリを 1 本だけ起こす。`wait_ret` は
  `OP_WAIT` の戻り値としてアプリに渡る。戻り値は app_id (また park した) / 0 (終了した) / <0。
  起こせるのは **`OP_WAIT` で park された印のあるフレームだけ**で、印が無ければ
  `OS32_ERR_STALE` を返し `ring3_resume_bad_frame_count` が増える (受入 G7)。
- `exec_park()`: 走っているアプリを `OP_WAIT` の中で止め WM へ戻す。成立すれば **戻らない**。
  呼べない文脈 (他の op / 走っているアプリが居ない / CUI の入れ子の子) では `OS32_ERR_INVAL` を
  返して普通に戻り、`ring3_park_reject_count` が増える。呼んでよいのは gshell の `op_wait` の
  ループ先頭 1 点だけ (park 規約)。
- `exec_kill(app_id)`: 止めてあるアプリを起こさずに畳む。走っている本人は `OS32_ERR_STALE`
  (そちらは CTRL+STOP の経路)。
- `exec_app_state(app_id)`: 0 = 空き / 1 = 走っている / 2 = park 中。
- `snd_focus(app_id)`: 音の所有者をフォーカス窓の owner に合わせる。それまでの所有者の BGM を
  退避して止め、移った先に退避があれば復元する。**同時には鳴らさない**。フォーカスの無い
  所有者の `snd_bgm_play` は鳴らさず退避に積むだけ、SE は捨てる。

受入 G7 のカウンタ (`ring3_switch_count` / `ring3_transition_count` / `ring3_park_reject_count` /
`ring3_resume_bad_frame_count`) は **KAPI ではなくカーネルシンボル**で、`fault_kill_count` と
同じく `kernel.map` の番地を `emu_read_mem` で読む。

- `kbd_trygetrawkey`: 戻り値 `keycode | down<<8 | mods<<9` (mods = そのイベント時点の `SHIFT_*`、GUI モード中のみ記録)、無ければ -1。WM (gshell) が Key down/up を作る。
- `ime_feed_key(keydata)`: WM が打鍵 `(scancode<<8)|ascii` を FEP に通す (GUI 中はカーネルが cooked に積まないため)。負 = 確定文字列の続きだけ。戻り値: <0 消費 / >=0x100 素通り / 0x1B ESC 素通り / 1..0xFF 確定 UTF-8 の 1 バイト (W2)。
- `ime_set_render(table)`: FEP の描画バックエンド (`IME_Render` 関数表) を差し替える。NULL で TVRAM 版へ戻す。gshell が GFX 版を渡す (W2)。

`gfx_screen_info` の `out` は `GFX_ScreenInfo` (`os32_kapi_shared.h`): 画面サイズ・bpp・
画素形式 (`GFX_FMT_*`)・能力ビット (`GFX_CAP_*`)・パレットリース範囲。GUI とアプリは
これを信じて 400 ライン・16 色を決め打ちしない。`gfx_hw_fill_rect` / `gfx_hw_blit` は
アクセラレータ系バックエンド用の枠で、CPU バックエンド (9801 プレーン / PEGC) では
`OS32_ERR_NOSYS` (-10) を返す。呼ぶ側は `GFX_CAP_HW_FILL` / `GFX_CAP_HW_BLT` を見て
CPU 実装へフォールバックする。設計: `docs/tasks/gui/API_CONTRACTS.md` G5 / DESIGN.md §7。

`ime_user_list` の `out` は `IME_UserEntry`(`sdk/include/os32/os32_kapi_shared.h`) の配列。
`ime_trygetkey` は FEP を通したノンブロッキングのキー取得で、`kbd_trygetkey` の
FEP 対応版にあたる (エディタ等のメインループから使う)。

### CTRL+STOP の宛先切り替え (v45)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x2F0 | exec_abort_clear | `i32(void)` |

- `exec_abort_clear()`: IRQ1 が立てた CTRL+STOP の要求を降ろす。**owner 1 (シェル帯) からのみ**で、
  それ以外は `OS32_ERR_INVAL`。0 = 降ろした / 要求が無かった。
  IRQ1 の時点でカーネルが知っているのは「いま走っているアプリ」だけなので要求はそこに立つが、
  契約 T6 の宛先は**フォーカス窓のアプリ**。WM (gshell) はフォーカス窓の owner が走っている本人で
  なければ、これで本人の要求を降ろしてからフォーカス窓の ID を `exec_kill` で畳む。降ろさないと
  本人が次の syscall 境界 (`ring3_abort_check`) で畳まれ、**意図しない 1 本が死ぬ**。
  要求を負えるのは走っている 1 本だけなので対象は高々 1 本で、`abort_req` 以外
  (`state` / `in_op_wait` / `parked_from_wait`) と他の ID のスロットには触らない。
  WM が top-level (owner 1) に戻るのは park の後なので、対象のスロットは PARKED になっている
  ことがある — 状態では絞らない。

### console シンクと実 RAM 量 (v46)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x2F4 | con_sink_read | `i32(void *buf, u32 cap)` |
| 0x2F8 | con_sink_stat | `i32(u32 *pending, u32 *dropped)` |
| 0x2FC | sys_ram_kb | `u32(void)` |

### 打鍵の注入 (v47)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x300 | kbd_inject | `i32(const u8 *utf8, u32 len)` |
| 0x304 | kbd_inject_pending | `u32(void)` |

GUI モード中 (gshell が全画面 GFX を握っている間) は IRQ1 が cooked リングに積まないので、
CUI プログラムが `kbd_getchar()` を呼ぶとカーネルの `hlt` ループに入って**戻らない** —
syscall の中なので `exec_park` も起きず、協調型の全体が止まる。v47 はこれを 2 つで解く。
票 [tasks/gui/v13/TASK_K7_input.md](tasks/gui/v13/TASK_K7_input.md) §1 / §5。

- `kbd_inject(utf8, len)`: UTF-8 のバイト列をカーネルの**注入リング (256B、静的)** へ積み、
  積んだバイト数を返す。呼べるのは **con_sink の読み手** (= 端末アプリ) だけで、読み手が
  未確立か別の所有者なら `OS32_ERR_EXIST`、`utf8 == NULL` は `OS32_ERR_INVAL`。
  端末アプリはイベントループに入る前に `con_sink_read` を 1 回呼んで読み手権限を確立する
  規約 (票 §5 R2)。あふれは con_sink と逆で **新しい方を捨てる** — 打鍵は順序が意味を持ち、
  古い方を捨てると打った文字列の頭が欠ける。捨てた分は戻り値 (< `len`) で分かり、累計は
  カーネルシンボル `kbd_inject_drop_count` にも積む (KAPI にはしない)。
- `kbd_inject_pending()`: 未読バイト数。所有権は要らない — WM (gshell) が
  「`exec_app_state` が 3 (`WAIT_KEY`) かつ `kbd_inject_pending() > 0`」で起こす相手を選ぶ。
- GUI 中の `kbd_getchar` / `kbd_getkey` / `kbd_trygetchar` は**この注入リングだけ**を見る。
  `kbd_getchar` / `kbd_getkey` は空のとき **第 2 の park 点**として止まり
  (`APP_STATE_WAIT_KEY` + 印 `parked_from_kbd`)、`exec_resume` が注入リングの 1 バイトを
  保存フレームの EAX に入れて起こす (WM の `wait_ret` は使わない)。リングが空のままの
  `exec_resume` は `OS32_ERR_AGAIN` で、印も状態も残る。
  `kbd_getkey` の GUI 中の戻り値は下位 8bit だけが意味を持つ (スキャンコードは 0)。
- **CUI モード (`kbd_gui_mode == 0`) は 1 行も変わらない** — rshell のタイムアウト経路を
  含めて従来どおり cooked リング + `hlt`。
- 注入リングは CUI 復帰 (`console_text_gdc_start`) と読み手の退場
  (`exec_reclaim_owned` → `kbd_inject_owner_exit`) で捨てる。

GUI モード中 (gshell が全画面 GFX を握っている間) は、カーネルと CUI コマンドが
`kernel/console.c` の入口へ書いた出力はテキスト VRAM に描かれて見えないまま消える。
v46 はそれを**カーネル内の 8KB のリング (シンク)** に溜め、端末アプリ (外部、K6C-A) が
吸って描けるようにする。有効化の API は無い — `console_text_gdc_stop()` (GUI 入場) で
空から溜め始め、`console_text_gdc_start()` (CUI 復帰) で捨てる。票
[tasks/gui/v13/TASK_K6C_console.md](tasks/gui/v13/TASK_K6C_console.md) §2。

- **レコード形式** (`os32_kapi_shared.h` の `CON_SINK_*` が正典)。先頭 1 バイトが型で、
  残りは型ごと。詰め物も整列も無い:

  | type | ワイヤ | 発生源 |
  |---|---|---|
  | `CON_SINK_REC_PRINT` (1) | `[1][color u8][len u8][UTF-8 len バイト]` | `shell_putchar` / `shell_print` / `shell_print_utf8` / `console_write` / `kprintf` |
  | `CON_SINK_REC_CLEAR` (2) | `[2]` | `tvram_clear()` |
  | `CON_SINK_REC_CURSOR` (3) | `[3][x u8][y u8]` | `console_set_cursor()` |

  改行 / CR / TAB は `PRINT` のバイトとして流れる (端末モデルが解釈する)。スクロールは
  レコードにしない。`PRINT` 1 本は `CON_SINK_PRINT_MAX` (200) バイトまでで、長い出力は
  **UTF-8 の切れ目で**分割される。レコード 1 本の最大は `CON_SINK_REC_MAX` = 203。

- `con_sink_read(buf, cap)`: 溜まっているレコードを**境界で切って** `buf` へ写し、
  書いたバイト数を返す (0 = 空)。`cap` は `CON_SINK_REC_MAX` 以上でなければならず、
  足りなければ `OS32_ERR_INVAL` (0 を返すと「空」と区別できず読み手が止まる)。
  **読み手は 1 本だけ**で、最初に読んだ所有者が持つ。別の所有者からは `OS32_ERR_EXIST`。
  所有は `exec_exit` / `exec_kill` の owner 回収 (`con_sink_owner_exit`) でだけ返る —
  CUI 往復では動かない。
- `con_sink_stat(pending, dropped)`: 溜まっているバイト数と、あふれで捨てたレコード数。
  常に 0 を返す。所有権は要らない (誰でも覗ける)。CPL=3 からは NULL を渡せない
  (ディスパッチャのポインタ検証で kill される)。
- あふれは**古い方をレコード単位で捨てる**。捨てた回数はカーネルシンボル
  `con_sink_drop_count` にも積む (KAPI にはしない)。
- `sys_ram_kb()`: 起動時に実際に登録した物理 RAM span の合計 (KB)。`sys_get_mem_kb()` は
  RAM 上端アドレス / 1024 なので PC-98 の 15-16MB システム空間を RAM として数えるが、
  こちらは数えない (15MB 機が 17408 KB を名乗る問題。K6-RAM 決裁 (2))。RAM が 15MB より
  下で止まる機械では両者が一致する。実体は `kernel/memory_boot.c` の `memory_boot_ram_kb()`。

### 画面の所有者 (v48)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x308 | gfx_screen_owner | `i32(void)` |

全画面 GFX は「1 枚の画面を丸ごと持っていく」操作なので、**持ち主をカーネルが 1 つだけ覚える**。
票 [tasks/gui/v13/TASK_T8_fullscreen_gfx.md](tasks/gui/v13/TASK_T8_fullscreen_gfx.md) §2 の D1 / D1a / D3。

- `gfx_screen_owner()`: いま画面を握っている ID。**1 = シェル帯 (WM)** / 2〜5 = アプリ。
  誰でも呼べる (所有権は要らない)。WM (gshell) は `exec_start` / `exec_resume` から戻った
  直後にこれを見て、1 以外なら全画面モードに入り (合成も present もしない)、1 に戻っていれば
  復帰の描き直しに入る。実体は `exec/appslot.c` の `g_gfx_owner`。
- **取る**のは `gfx_init` / `gfx_init_200` の KAPI ラッパだけ。GUI 中 (console シンクが有効な間)
  に CPL=3 の非シェルが呼ぶと、所有者がその ID へ移る。CUI 中は所有者を触らない (常に 1) —
  gshell が居らず、全画面は従来どおり誰でも取れる。gshell 配下の GUI アプリは
  `libos32gfx_attach()` で取り付くだけで `gfx_init` を呼ばないので、所有者にはならない。
- **返す**のは回収 (`exec_reclaim_owned`) だけ。正常終了 / `exec_kill` / fault / CTRL+STOP の
  どの経路で畳まれても、所有者がその ID なら 1 (WM) へ戻る。FD / SHM / DB / GUI 窓と同じ並び。
- **宣言なしは断る (D1a)**: GUI 中に `OS32X_FLAG_GFX` (`mkos32x --gfx`) の立っていない CPL=3 が
  `gfx_init` / `gfx_init_200` を呼んだら、**本体を呼ばずに何もしない**。`gfx_init` は `void` なので
  戻り値では知らせられない — 断った回数はカーネルシンボル `gfx_init_reject_count` に積む
  (KAPI にはしない)。CUI 中は従来どおり何でも通す。
- **`--cpl0` は GUI から起動させない (D1)**: `OS32X_FLAG_FORCE_CPL0` のプログラム (v86 / VDM 系) は
  VRAM を直接触るので、画面の所有者の外側で画面を壊す。`exec_start` (GUI 経路) は生存アプリの
  有無に関わらず `OS32_ERR_INVAL` で断る。CUI の `exec_run` は従来どおり (生存アプリが 1 本でも
  居れば `OS32_ERR_FULL`、居なければ通す)。
- `gfx_init` を呼ばずに VRAM へ直接書く CPL=3 プログラムは「行儀の悪いプログラム」として扱い、
  カーネルは守らない。WM の present を捨てる保険 (D2) は落とした (2026-09-12 ユーザー決裁)。


### 起動要求表 (v49)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x30C | launch_req | `i32(const char *cmdline)` |
| 0x310 | launch_pending | `i32(void)` |
| 0x314 | launch_take | `i32(char *buf, u32 cap, i32 *requester, i32 *kind, i32 *arg)` |
| 0x318 | launch_report | `i32(i32 token, i32 rc)` |
| 0x31C | launch_poll | `i32(i32 token, i32 *status)` |
| 0x320 | launch_cancel | `i32(i32 token)` |
| 0x324 | launch_child | `i32(i32 id)` |
| 0x328 | sys_yield | `i32(void)` |

### 設定レジストリの基盤 (v50)

既存 `db_*` 10 本 (0x238〜0x25C) は 1 つも動かない ([ABI2])。追加は末尾だけで、
新しい handle にも既存の `db_step` / `db_finalize` / `db_close` をそのまま使う
(反復は再 prepare。reset API は足さない)。票
[tasks/settings/TASK_S0.md](tasks/settings/TASK_S0.md) §1a、実体は `kapi/kapi_db.c`。

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x32C | db_open_existing | `int(const char *path, int writable)` |
| 0x330 | db_prepare_only | `int(int handle, const char *sql)` |
| 0x334 | db_bind_int | `int(int handle, int index, int value)` |
| 0x338 | db_bind_text | `int(int handle, int index, const char *text, int length)` |
| 0x33C | db_bind_blob | `int(int handle, int index, const void *data, int length)` |
| 0x340 | db_bind_null | `int(int handle, int index)` |
| 0x344 | db_error_code | `int(int handle)` |

### Host Services (v51)

要求 1 本 = ハンドル 1 本。**どれも待たない** (呼び手は `OS32_ERR_AGAIN` を見て
`sys_yield` / `sys_halt` で再試行する)。プロトコルを進めるのは 100Hz の
`link_tick()` だけで、KAPI は状態を読み書きするだけ。契約の正典は票
[archive/network/TASK_N0.md](archive/network/TASK_N0.md) §1a、実体は
`kapi/kapi_host.c` (検証と写し) + `net/link.c` (状態機械)。

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x348 | host_open | `i32(const char *req, u32 len)` |
| 0x34C | host_status | `i32(i32 h, u32 *status, u32 *length)` |
| 0x350 | host_read | `i32(i32 h, void *buf, u32 cap)` |
| 0x354 | host_write | `i32(i32 h, const void *buf, u32 len)` |
| 0x358 | host_close | `i32(i32 h)` |

### 更新日時の保存 (v52)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x35C | sys_set_mtime | `int(const char *path, u32 mtime)` |

- `mtime` は **UNIX Epoch 秒 (UTC)**。`0` は現行 ABI の「不明」の印なので `INVAL` で断る
  (`OS32_Stat` に時刻の有効性ビットが無いため。不明を書けると次の同期で「証拠が無い」状態を
  自分で作ることになる)。`path` は NUL 終端で `OS32_MAX_PATH` 未満 — 溢れたら**切り詰めずに**
  `INVAL` (切り詰めた別のパスの時刻を動かさない)。CPL=3 からは 1 バイトずつ範囲を確かめながら
  カーネル側へ写す (`kapi/kapi_sys.c`)。
- `VfsOps.set_mtime` は**任意実装**。埋めていない FS ドライバでは `OS32_ERR_NOSYS` が返る。
  これは失敗ではなく「この FS には無い」という答えで、呼び手は内容の同期を続けたまま
  「時刻の保存を省略した」と表示する。**実装済みは ext2 だけ** (FAT / HostDrv / iso9660 は NOSYS)。
- ext2 は inode の `mtime` を与えられた値に、`ctime` を**ゲスト側の現在時刻**にする
  (`ctime` は作成時刻ではなく inode の状態変更時刻)。`atime` は触らない。
  inode を書いたあと `ext2_sync()` まで通すので、成功は「媒体へ出した」を意味する。
- **データを書き終えてから呼ぶこと。** 通常の書き込みは `mtime` を現在時刻で上書きするので、
  先に設定すると消える (設計書 [HSYNC_IMPROVEMENT_PLAN.md](tasks/shell/HSYNC_IMPROVEMENT_PLAN.md) §5.2)。

### 覗くだけのキー取得 (v54)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x360 | kbd_peekkey | `int(void)` |

- 戻り値は `kbd_trygetkey` と同じ形 (上位 = スキャンコード、下位 = ASCII。GUI 中は下位 8bit
  だけで、スキャンコードは 0)。キューが空なら `-1`。
- **キューを動かさない。** 同じキーを何度覗いても同じ値が返り、取り出すのは `kbd_trygetkey`
  (または `kbd_getkey` / `kbd_getchar`) を呼んだときだけ。「ESC かどうかだけ見て、ESC でなければ
  次の読み手に残す」という監視 (`source` 中の打ち切り) がこれで書ける。

### 直前の同期起動の結果 (v55)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x364 | exec_last_result | `int(int *kind, int *code)` |

- 直前の **`exec_run`** (塞ぐ同期起動) の結果を「種別 + 値」で返す。戻り値は
  `0` = 記録あり / `OS32_ERR_INVAL` = 記録なし。記録が無いときは `*kind = EXEC_KIND_NONE`、
  `*code = 0` に揃えるので、呼び手が前の値を読み続けることはない。
- **種別は値から作れない** — `exit(-2)` と fault はどちらも `exec_exit_status = -2` になる。
  だから種別は畳んだ側が渡す: `kapi_sys_exit` → `EXEC_KIND_EXITED`、`exec_fault_recover` →
  `EXEC_KIND_FAULT`、CTRL+STOP (`ring3_abort_check`) → `EXEC_KIND_ABORTED`。
- **`exec_run` はすべての return 点で記録を書く。** 起動そのものに失敗した経路
  (`exec_launch` の早期 return) は `exec_exit` を通らないので、`exec_run` が戻り値を
  `EXEC_KIND_NOT_FOUND` / `INVALID` / `NOMEM` / `GENERAL` へ写す。成功の直後に未知の
  コマンドを起動しても前回の記録は返らない。
- `appslot_start_admit` の `OS32_ERR_FULL` / `OS32_ERR_INVAL` は `EXEC_KIND_NOMEM` に寄せる
  (= 呼び手が「次の候補へ進まない」と読める側)。
- **GUI 経路 (`exec_start` / `exec_resume`) の子は記録しない** (`AppSlot.gui`)。
  読み口である `sh.bin` はカーネルの記録を読まないので、書くと同期起動の結果と紛れるだけ。
- 源の見る順番は `kbd_trygetkey` と同じ — GUI の注入リング → rshell のシリアル → cooked リング。
- **`exec_park_poll` を呼ばない** (= WM へ譲らない)。park は成立すると戻らず、起こされるときに
  `exec_resume` が注入リングの 1 バイトを取り出して EAX に入れてしまうため、「覗いただけ」に
  ならない。譲りたい呼び手は従来どおり `kbd_trygetchar` / `kbd_trygetkey` を使う。

### 実機のシリアルを 115200bps へ (v56)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x368 | serial_init_vfast | `int(u32 baud)` |
| 0x36C | serial_get_status | `int(u32 *mode, u32 *baud, u32 *fifo)` |

- **既存の `serial_init` (0x0BC) は意味も引数も変えていない。** 従来どおり互換モード
  (8251 + 8253 カウンタ#2) で初期化する。起動時の既定 9600 がこの経路を通る
  (票 TASK_SERIAL_VFAST の決裁「実機で通った経路を守る」)。
- `serial_init_vfast` は **V･FAST モード (`013Ah` bit7)** で初期化する。入れるのは
  **FIFO 搭載機 (`0136h` bit6 の反転で判定) かつ資料の表にある速度**
  (9600 / 14400 / 19200 / 28800 / 38400 / 57600 / 115200) のときだけ。
  戻り値は 3 つ:

  | 値 | 意味 |
  |---|---|
  | `0` | V･FAST に入った |
  | `-1` | 互換モード (8253) で初期化した。頼んだ速度がちょうど出ている |
  | `-2` | **8253 でもちょうど出せないので何もしなかった。** いまの設定がそのまま生きている |

- **`-2` は「拒否」であって「ずれたまま適用」ではない。** 以前はずれた実効値を
  そのまま入れていたが、FIFO 非搭載機で `serial 115200` を打つと 2.4576MHz 系では
  count=1 = 153600bps が入り、ホストが 115200 へ移ったきり**戻すための
  `serial 9600` も届かなくなる**。ハードウェアには 1 バイトも書かずに戻るので、
  呼び手は速度を変えていない前提で続けてよい。`serial_init` も同じ判断をする
  (戻り値が無いので結果は `serial_get_status` で見る)。
- **切替の全体は IRQ4 と IF を落として行う。** `ser_head`/`ser_tail`/`ser_count` の
  初期化とポート・ビットマスクの差し替えの途中で受信 IRQ が入ると、古いバイトが
  1 つ残ったり新しいポートを古いビット位置で読んだりする。
- V･FAST 中はデータが `0130h`、ステータス/コマンドが `0132h` になり、**ステータスの
  ビット位置が互換と違う** (`0032h` は bit0 TxRDY / bit1 RxRDY、`0132h` は
  bit1 TxRDY / bit2 RxRDY)。これはカーネル内で閉じているので、KAPI の呼び手からは
  `serial_putchar` / `serial_trygetchar` が従来どおり使える。
- 戻しは `serial_init(9600)`。`013Ah` bit7 = 0 と `0138h` = 0 を**8251 を触る前に**書く。
- `serial_get_status` は `mode` = `0` 互換 / `1` V･FAST、`baud` = **実効値** (要求値では
  ない)、`fifo` = `0136h` の判定を返す。`NULL` は飛ばす。戻り値 `0` = 初期化済み /
  `-1` = まだ `serial_init` を呼んでいない。**カーネルのポインタは返さない**
  (POLICY_DEBUG §4-13 の `db_last_error` と同じ事故を繰り返さないため)。
- 資料は `docs/hw/undocumented/io_rs.md` (`0130h`〜`013Ah`)。**NP21/W は通信速度を
  模擬しない**ので、分周が合っているかはエミュレータでは確かめられない
  ([tasks/realhw/TASK_SERIAL_VFAST.md](tasks/realhw/TASK_SERIAL_VFAST.md))。

### ローカル打鍵だけの読み口 (v57)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x370 | kbd_trygetchar_local | `int(void)` |

- **cooked リングだけを見る。** `rshell_active` でもシリアルを見ず、GUI 中の
  注入リングも見ない (GUI 中の cooked リングは常に空なので `-1`)。
  取れたら 0..255、無ければ `-1` — 戻り値の形は `kbd_trygetchar` と同じ。
- **`exec_park_poll` を呼ばない** (WM へ譲らない)。由来を知るためだけの口で、
  待つ意図は呼び手の側にある。
- 何のためか: `kbd_trygetchar()` は rshell 中にシリアルも見るので、
  呼び手は「この 1 バイトがどこから来たか」を知れない。rshell の速度切替の
  番犬は**シリアル由来の行だけ**を往復の証拠に数えるので、
  「シリアルを 1 回読む → 空ならローカルを 1 回読む」と書ける口が要る。
  2 度読みのあいだに届いたバイトが由来の印を落とす窓が消える
  ([tasks/realhw/TASK_SERIAL_VFAST.md](tasks/realhw/TASK_SERIAL_VFAST.md) 往復 3 ④)。
- 同じ日に L-A (PCI 列挙) も版を取ったので、**着地時にそちらを v58 へ送った**。この節のスロット 0x370 はそのまま。

**併せて `serial_putchar` (slot 38 = 0x98) の戻り値を `void` → `int` に広げた。**

| 値 | 意味 |
|---|---|
| `0` (`KAPI_SER_TX_OK`) | UART へ書けた |
| `-1` (`KAPI_SER_TX_DROPPED`) | TxRDY の予算を使い切って諦めた (相手が読んでいない) |

- **スロット番号も引数の並びも変えていない** ([ABI2])。cdecl では戻り値は
  EAX で返り、呼び手が無視しても害が無いので、**古いバイナリを新しい
  カーネルで動かしても壊れない** (EAX は呼び手の scratch)。逆向き
  (新しいバイナリを古いカーネルで) は `build/app.conf` の要求版数が止める。
- 何のためか: rshell の速度切替の番犬は「応答の EOT を**送り終えた**」ことを
  往復の証拠にしている。送信を諦めたのに成功と数えると「応答したつもり」で
  解除してしまう (往復 3 ③)。既存の呼び手 (`userland/lib/rt/dbgserial.c` など)
  は戻り値を無視してよい。
### 実機の PCI 列挙 (v58)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x374 | pci_count | `int(void)` |
| 0x378 | pci_get | `int(u32 idx, void *out)` |
| 0x37C | pci_cfg_read32 | `u32(u32 bus, u32 dev, u32 fn, u32 reg)` |

- 起動時に `pci_init()` (`kernel.c`、**`ide_init()` の直前**) が 1 回だけ走り、
  コンフィギュレーションメカニズム #1 で bus 0 を走査する。ブリッヂの配下は
  深さ 2 まで、しかも **secondary バス番号が現在のバスより大きいとき**だけ潜る。
- `pci_count` は記録したデバイス (= ファンクション) の数。**PCI が無い機械では 0**。
  PCI のある機械には必ずホストブリッヂが居る (`io_pci.md` 55 行) ので、
  呼び手は `0` を「PCI 無し」と読んでよい。
- `pci_get` は idx 番目の記録を **呼び手のバッファへ写す**。並びは
  `drivers/pci.h` の `struct pci_dev` と同じで、i386 で **40 バイト固定**
  (両側の `STATIC_ASSERT` が見張る)。**カーネルのポインタは返さない**
  (POLICY_DEBUG §4-13 の `db_last_error` と同じ事故を繰り返さないため)。

  | Offset | 型 | 名前 |
  |---|---|---|
  | 0 / 1 / 2 | `u8` | bus / dev / fn (3 の位置は padding) |
  | 4 / 6 | `u16` | vendor / device |
  | 8 / 9 / 10 / 11 | `u8` | class / subclass / progif / header (Header Type は bit7 込みの生値) |
  | 12〜35 | `u32 [6]` | BAR0〜5 の **生値** (復号しない) |
  | 36 / 37 | `u8` | irq_line (config 0x3C) / irq_pin (0x3D) |
  | 38 | `u16` | command (config 0x04) |

- `pci_cfg_read32` は config 空間の生読み (`pcidump` が 256 バイトを吸い出す)。
  `reg` の下位 2 ビットは落ちる。**書きは KAPI に出していない** — BAR に
  全 1 を書くサイズ判定も含めて、BIOS の割り当てを壊す操作は L-A の範囲外。
- **BAR の生値 `0x00000001` は「無い」ではない。** I/O BAR はあるが番地が
  割り当てられていない状態で、L-B (82557 ドライバ) が自分で割り当てるかを
  決める材料になる (票 §5-2 R1)。
- **NP21/W は `0CF8h` を実装していない。** `pci_init()` は読み戻しが一致しない
  ことを見て `[pci] mech#1 absent` を出し、その場で戻る。これは失敗ではなく
  正しい報告 ([V4])。復号の側はホスト試験 (`make check-pci-decode-host`) で
  固めてある — 記録は `tools/tests/pci_decode_tdd.md`。
- 資料は `docs/hw/undocumented/io_pci.md` (図2 = アドレス語、446〜479 行 =
  `0CF8h` / `0CFCh`) と Intel 8255x SDM Table 1 (Type 0 ヘッダ)。

### µs 時計 (v59)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x380 | sys_time_now | `int(u32 *lo, u32 *hi)` |

- `us = tick × period_us + (reload − count) × period_us ÷ reload`。積は **64 ビット**で
  組む (u32 だと 429496 tick = **71 分**で時刻が 0 に戻る)。`tick_count` の u32 周回
  (497 日) は扱わない (契約)。
- **スナップショットの採り方** (票 §1-5 R8、全体を `irq_save` の中で):
  ラッチ**前**の PIC1 IRR bit0 (`p1`) → カウンタ#0 をラッチして下位 → 上位 →
  ラッチ**後**の IRR bit0 (`p2`) → `tick_count`。判定は
  `p1 == 0 && p2 == 0` → `t` / `p1 == 1` → `t + 1` (再ロードはラッチより前。
  count は新しい周期のもの) / `p1 == 0 && p2 == 1` → **やり直し** (最大 3 回)。
- **正しいのは IRQ0 が失われない範囲** = システム全体で IF=0 の区間が 1 周期
  (10ms) 未満のとき。2 回以上の境界を IF=0 で跨ぐと tick が 1 つ落ち、この時計も
  `tick_count` も 10ms 遅れる — 検出はしない。呼び手は `tick_count × 10000` に
  落とせるが、その値は同じ tick の補間値より**小さい**ので、fallback は単調性の
  保証に含めない (呼び手が前回値と max を取る)。
- CPL=3 の検証は 2 段。(1) 生成される `kapi_argptr` → `ring3_ptr_ok` が**先頭番地の
  帯**を見て、外れていれば `ring3_fault_kill` (他の KAPI と同じ)。(2) `kapi_sys_time_now`
  が NULL・4 バイトの帯境界跨ぎ・**2 本の範囲の交差**を `OS32_ERR_INVAL` で断り、
  **書く前に**両方の出力ページが present + RW + USER であることを
  `ring3_user_ranges_writable` で確かめる (落ちたら kill)。
  **OS32 は CR0.WP = 0 で走る** (`kernel/shlib.c` がカーネルからの書き込みに依存)
  ので、読み取り専用の USER ページ = 共有ライブラリの `.text` へ CPL=0 から書いても
  #PF は起きない。帯の検証だけでは止まらない。
- 表を歩くときは **master CR3 へ切り替えてからアプリ PD の物理を読む**。アプリ CR3 の
  まま辿ると、PD もアプリ PT も `PGALLOC_BASE` (= アプリ帯) から取られているため
  **表のつもりでアプリ自身のデータを読む** (2026-09-13、実機 K2 で 2 回失敗)。
  `paging_pte_flags()` も使えない (master の PT を引くので、アプリでは RW + USER の
  shlib `.data`/`.bss` が master では USER 無しに見え、正常な出力を誤って拒否する)。
  2 本の出力は**1 回の往復でまとめて**見る (1 本ずつだと CR3 の書き込みが 4 回になる)。

### PCI 結線の診断 (v60)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x384 | pci_bind_info | `int(u32 idx, void *out)` |

- `idx` は **`pci_get` (v58) と同じ列挙順**。`pci_count()` 件まで。
- 写すのは **8 バイトちょうど** (`PCI_BIND_INFO_SIZE`)。既存の `pci_get` の
  40 バイトは**広げない** — 旧呼び手のバッファをはみ出すので別の口にした。

  | Offset | 型 | 名前 |
  |---|---|---|
  | 0 / 1 / 2 | `u8` | bus / dev / fn |
  | 3 | `u8` | result — NONE 0 / BOUND 1 / DECLINED 2 / QUARANTINED 3 |
  | 4 | `u8` | irq — 列挙時の `irq_line` (0xFF = 未割り当て) |
  | 5 | `u8` | reason — OK 0 / NO_DRIVER 1 / IRQ_UNSUPPORTED 2 / IRQ_QUARANTINED 3 / RESET_FAILED 4 / START_FAILED 5 / NOISY 6 / NOISY_UNMASKABLE 7 / DECLINED_UNSPECIFIED 8 |
  | 6 | `u8` | line_state — OK 0 / STORM_MASKED 1 / QUARANTINED 2 |
  | 7 | `u8` | pad (常に 0) |

- **`line_state` は保存値ではない。** `pci_bind_info_get()` が読む時点で
  `irq_line_quarantined` / `irq_storm_masked` から合成する (票 §1-4 往復 9 R1)。
  先に BOUND した装置の線が後から別の装置のせいで隔離されても `result` は
  BOUND のままで、`line_state` = QUARANTINED が「IRQ が来なくなった」を伝える。
  **両方立っていれば QUARANTINED** (ストームのマスクは再計算で外れ得るが、
  隔離は再起動まで戻らない。弱いほうを名乗ると復旧済みに見える)。
- CPL=3 の検証は v59 と同じ 2 段。(1) 生成される `kapi_argptr` → `ring3_ptr_ok`
  が先頭番地の帯を見る。(2) `kapi_pci_bind_info` が NULL と 8 バイトの帯境界跨ぎを
  `OS32_ERR_INVAL` で断り、**書く前に** `ring3_user_ranges_writable` で
  present + RW + USER を確かめる (落ちたら `ring3_fault_kill`)。
  **OS32 は CR0.WP = 0** なので、共有ライブラリの `.text` を渡されても
  ハードウェアは止めない。
- `idx` が範囲外なら `OS32_ERR_INVAL` で、**出力は 1 バイトも書かない**。
- シェルの `lspci` が 1 行の末尾に注記を足す: `bound (ok) irq=N` /
  `declined (<reason>)` / `quarantined (<reason>)` と、線の様子の
  `[irq N quarantined]` / `[irq N storm-masked]`。`result` が NONE で
  `line_state` が OK のとき (= 一致する driver が無いふつうの装置) は何も出さない。
- **NP21/W には PCI が無い**ので、エミュレータではこの口は常に 0 件
  (`lspci: no PCI`)。規則はホスト試験 `make check-pci-bind-host` が固める。

### CS4231 (MATE-X PCM) の再生 (v61)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x388 | pcm_open | `int(u32 rate)` |
| 0x38C | pcm_write | `int(const void *buf, u32 bytes)` |
| 0x390 | pcm_status | `int(u32 *free_bytes, u32 *counters)` |
| 0x394 | pcm_close | `int(void)` |
| 0x398 | pcm_set_volume | `int(u32 percent)` |

16 ビット・ステレオ・**44.1k と 22.05kHz だけ**の再生。単位は **frame**
(左右 1 組 = 4 バイト)。録音・ミキサ (出力減衰以外)・PIO・V86 への提供は無い。
設計の正典は [tasks/v3/TASK_PCM_CS4231.md](tasks/v3/TASK_PCM_CS4231.md)。

- **`pcm_open(rate)`** — `44100` / `22050` 以外は `OS32_ERR_INVAL`。
  戻り 0 / `OS32_ERR_NOSYS` (検出できない) / `OS32_ERR_FULL` (他の owner が
  open 中、または IRQ10 に結べない) / `OS32_ERR_NOSPC` (DMA プールか KHEAP) /
  `OS32_ERR_IO` (初期化の待ちが期限切れ。以後は再起動まで `OS32_ERR_IO`)。
  失敗は**逆順に巻き戻す** (IRQ を解除し、取れたメモリを返す)。
  経路レジスタ `0F40h` は `0x1A` (INT41 = IRQ10 + DMA #1) のまま残す —
  detach 状態で装置レジスタを書かないため。
- **`pcm_write(buf, bytes)`** — **frame の倍数だけ**受ける (端数は切り捨て)。
  戻りは**受け取ったバイト数**で、`0` は「ステージングが満杯」。呼び手は
  `sys_yield` して**もう一度**渡す (driver の中では待たない)。負は未 open・
  非 owner・範囲外。書き込むのはカーネルのステージング (16KB) で、
  **アプリのバッファを IRQ から読むことはしない**。
- **`pcm_status(free_bytes, counters)`** — `free_bytes` はステージングの空き
  (バイト)。`counters` = `(underruns << 24) | (repeats << 16) | resyncs`
  (8 / 8 / 16 ビット、255 / 255 / 65535 で飽和)。**出力 2 本**で、
  検証は v59 と同じ規則 (NULL は見ない / 書く前に present + RW + USER)。
  drain の失敗はここには出ない — `pcm_close` の戻り値で受ける。
- **`pcm_close()`** — **drain、期限つき**。残りを鳴らし切り、最後のデータの
  半分と無音の半分を通してから止める。期限は
  `(ceil(staged / 2048) + 3) × 半周期 + 3 tick` (44.1k・staged 0 で 17 tick)。
  0 = 鳴らし切った / `OS32_ERR_IO` = 途中で止めた (番犬・連続性の喪失・期限)。
  **「PI が来ない」は止まった証拠にしない** — 見るのは PEN=0 と I24 の PI=0。
- **`pcm_set_volume(percent)`** — 1〜100 を I6/I7 の 6 ビット減衰へ線形に写す
  (100 = 0dB)。**0 は D7 のミュート**。101 以上は `OS32_ERR_INVAL`。
- **所有者**は既存の資源 owner と同じアプリ ID (`res_owner_get()`)。
  `pcm_write` / `pcm_status` / `pcm_close` / `pcm_set_volume` は一致を要求し、
  不一致は `OS32_ERR_INVAL`。異常終了 (fault / CTRL+STOP / kill) は
  `exec_reclaim_owned` の `pcm_reclaim` が**待たずに** PEN=0 → DMA マスク →
  解放まで進める (境界は問わない。捨てるストリームなので)。
- **装置が無い機械では `pcm_open` が `OS32_ERR_NOSYS`**。起動行は `[pcm] none`
  で、以後 tick は装置に 1 バイトも触らない。

### キーボード 8251 の診断 (v62)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x39C | kbd_diag | `int(KbdDiag *out)` |

実機で本体キーボードの打鍵が届かないときの切り分け用 (経緯と読み方は
[POLICY_DEBUG.md](POLICY_DEBUG.md) §4-57)。`KbdDiag` は
`sdk/include/os32/os32_kapi_shared.h` にあり、カーネルと外部プログラムで
同じ定義を使う (24 バイト、`drivers/kbd.c` の `STATIC_ASSERT` が見張る)。

| Offset | 型 | フィールド | 意味 |
|---|---|---|---|
| 0 | `u32` | `irq_count` | IRQ1 ハンドラに入った回数 (空・エラーも含む) |
| 4 | `u32` | `empty_count` | 0043h の RxRDY = 0 だった IRQ (0041h を読まずに返した) |
| 8 | `u32` | `err_count` | PE / FE のどれかが立っていた IRQ (0041h を読み捨て、ER 込みのコマンド語で解除。OE が重なってもこちら) |
| 12 | `u32` | `flushed` | `kbd_init` が起動時に読み捨てたバイト数 |
| 16 | `u8` | `init_st_before` | `kbd_init` がコマンド語を書く前の 0043h |
| 17 | `u8` | `init_st_after` | 書いた後の 0043h |
| 18 | `u8` | `last_st` | 直近の IRQ で読んだ 0043h |
| 19 | `u8` | `last_code` | 直近に受け取ったスキャンコード |
| 20 | `u8` | `cmd` | `kbd_init` が書いたコマンド語 (0x16) |
| 21 | `u8` | `now_st` | `kbd_diag` を呼んだ時点の 0043h |
| 22 | `u16` | `overrun_count` | OE だけが立っていた IRQ (0041h のバイトは正しいので使い、ER で解除)。0xFFFF で飽和。bda95fa では `reserved[2]` (= 0) だった場所で、大きさ・並びは変えていない |

- 戻り 0 = 成功 / `OS32_ERR_INVAL` = `out` が NULL。
- IRQ1 ハンドラが書く値 (u32 3 本・`last_st`・`last_code`) は割り込み禁止の
  間に一括で写すので、同じ瞬間の組になる。
- u32 のカウンタは飽和しない (折り返す)。`overrun_count` だけ u16 で飽和する。

### 排他的作成 (v53)

**スロットは増えていない。** `sys_open` に渡せるフラグが 1 つ増え、その意味が
広がっただけだが、**同じバイナリが古いカーネルで別の意味になる**ので版数を上げる
([ABI3])。定数は `sdk/include/os32/os32_kapi_shared.h` の手書き側にある
(`sdk/kapi.json` の構造体は変わらない)。

| 定数 | 値 |
|---|---|
| `KAPI_O_EXCL` | `0x0400` (`O_RDONLY` 0x00 / `O_WRONLY` 0x01 / `O_RDWR` 0x02 / `O_CREAT` 0x0100 / `O_TRUNC` 0x0200 と重ならない) |

- **`O_CREAT` と組でだけ有効。** 単独で渡すと `OS32_ERR_INVAL`。
- 名前が既に在れば**種別を問わず** `OS32_ERR_EXIST` — ディレクトリでも `ISDIR` ではない。
  呼び手は「予約名が在る」と「別の何かが在る」を区別する必要が無い。
- 在るかどうかを**判定できなかった**ときは、`NOTFOUND` 以外の負値を**そのまま返す**
  (票 B8 の「読めなかったを無いと読み替えない」)。
- `VfsOps.create_excl` は**任意実装**。埋めていない FS では `OS32_ERR_NOSYS`。
  「できなかった」ではなく「持っていない」という答えで、呼び手は**黙って通常の作成へ
  落ちてはいけない**。**実装済みは ext2 だけ** (HostDrv / FAT / ISO9660 は NOSYS)。
- **非対応 FS の判定は種別検査 (`vfs_path_kind`) と作成処理より先**に行う
  (`fs/vfs_fd.c`)。後ろに置くと `EXIST` / `ISDIR` / I/O エラーが `NOSYS` より先に返る。
- 排他性の根拠は **VFS が非再入で、ゲストが協調型**であること。
  **ホスト側が同時に書ける FS では成り立たない**ので HostDrv には実装しない。
- `O_TRUNC` を併せて渡しても何も起きない (作りたての長さ 0 に切り詰めるものが無い)。
- 契約と rename の置き換えの詳細は [06_filesystem.md](06_filesystem.md) §6-1。

- `host_open`: 要求行 1〜1400B (超過 / 0 → `INVAL`) をカーネル領域へ写し REQUEST を
  積む。HELLO 未確立 / 再同期中 → `STALE`、空き無し → `FULL`、直前のハンドルの
  RELEASE が未 ACK → `AGAIN`、NIC 無し / 未初期化 → `NOSYS`。戻り値は h (0 / 1)。
- `host_status`: 業務 RESPONSE 未着 → `AGAIN`。Agent が墓標を返したハンドル → `STALE`。
  出力ポインタは NULL 可で、**全部を先に検証してから書く** (失敗時は書かない)。
- `host_read`: 1 回に写す量は min(cap, リングの連続可用, 1400)。最後のバイトを写した
  呼び出しは正の長さを返し、**その次**の呼び出しが 0。リングの所有者は最初に読んだ
  ハンドルで、他方は `AGAIN`。
- `host_write`: 宣言長のある要求だけ (`ECHO <len>` / `CLIP PUT <len>` /
  `PRINT DATA <id> <len>` / `PUT ... <len>`)。`len > 残り宣言長` → `INVAL`
  (部分受付はしない)。REQUEST の転送 ACK 前と未 ACK の WDATA がある間は `AGAIN`。
- `host_close`: 任意の状態から解放し RELEASE を送る (bit0 の ACK まで再送)。
  **STALE のハンドルからは送らない**。二重 close → `INVAL`。回収は
  `exec_reclaim_owned` の `host_owner_exit`。

- `db_open_existing`: `writable` は 0 = `SQLITE_OPEN_READONLY` / 1 = `SQLITE_OPEN_READWRITE`。
  それ以外は拒否。**CREATE も URI も付けない** ので、無い DB は作られない。空 path /
  `:memory:` / `file:` 接頭 / `OS32_MAX_PATH` 超も拒否。open の**前**に `vfs_stat` で
  (1) 本体が存在し size > 0、(2) `<path>-journal` が**無い**ことを確かめ、反すれば
  **SQLite を呼ばずに**失敗する (RO でも hot journal の後始末が走るのを防ぐ)。
  診断は欠損 (stat が **NOTFOUND**) = `SQLITE_CANTOPEN`、0 バイト = `SQLITE_NOTADB`、
  journal あり = `SQLITE_BUSY_RECOVERY`。stat が NOTFOUND 以外で落ちたときは
  「journal の有無が分からない」ので `SQLITE_IOERR` で断る (不存在と同じ扱いにしない)。
  path は先に `vfs_resolve_path` で**絶対名へ解決**し、stat も open もその名前で行う
  (相対名を SQLite に渡さないので、transaction 中に cwd が動いても journal の削除先が
  変わらない)。ただし `vfs_resolve_path` は作業領域で**切り詰めてから**正規化するので、
  溢れた入力は「短い別の絶対名」として返る。だから解決の**前**に
  `kstrlen(cwd) + 1 + kstrlen(path) + 1` (絶対名は cwd 抜き) が `VFS_MAX_PATH` に
  収まることと、**成分数**が `VFS_MAX_PATH_DEPTH` (32) を超えないことを数え
  (resolver は溢れた成分を黙って捨て、その後ろの `..` が保持済みの成分を消す)、
  どちらかに反すれば `SQLITE_CANTOPEN` (path too long) で断る。`<絶対名>-journal` が下位層の path 容量 (`VFS_MAX_PATH` = 256B、SQLite の
  `mxPathname` も 256) に収まらないときは、journal の stat が切り詰められて**本体に
  当たる**ので open の前に `SQLITE_CANTOPEN` で断る。RW は `PRAGMA journal_mode` が `delete` で
  あることを照会だけで確かめ、**照会の失敗** (`SQLITE_NOTADB` / `IOERR` / `NOMEM` 等 —
  拡張コードを finalize の前に控える) と **照会は通ったが DELETE でない**
  (`SQLITE_CANTOPEN`) を区別して close し失敗する。失敗コードは **owner 別**の
  「直前 open 失敗」欄に残り `db_error_code(-1)` で読める。戻り値は handle / -1。
- `db_prepare_only`: SQL は NUL 込み `DB_SQL_MAX_BYTES` (1024B) 以内。超過は
  **切り捨てず拒否**する。単一の非空 statement のみ (末尾の空白 / コメント / `;` は可、
  次の statement があるかは `sqlite3_prepare_v2` の `pzTail` を**もう一度 prepare** して
  見る — stmt が返らなければ末尾は空白 / コメントだけ。空白やコメントの規則を自前で
  持たないので、判定は SQLite と 1 対 1 になる)。**step しない**ので、SELECT の先頭行も
  DML も進まない。同じ handle の旧 stmt は **拒否理由に依らず入口で** finalize して
  置換する (引数不正で断ったときに前の statement が残ると、次の `db_step` がそれを
  実行してしまう)。
- `db_bind_*`: prepare_only の後・**最初の step の前**だけ。`index` は 1-based。
  `text` は 0〜`DB_BIND_TEXT_MAX` (255) B、`blob` は 0〜`DB_BIND_BLOB_MAX` (4096) B。
  負・超過・NULL ポインタは拒否 (NULL 値は `db_bind_null`)。0B でも非 NULL の空値。
  カーネル側のスクラッチへ**検証付きでコピー**してから `SQLITE_TRANSIENT` で渡すので、
  SQLite が呼び手のポインタを保持することはない。
- `db_error_code`: slot の「最後の失敗」(SQLite 拡張 result code)。データ操作
  (`db_open_existing` / `db_prepare_only` / `db_bind_*` / `db_step` / `db_exec`) は
  成功で 0 に、失敗でそのコードに更新する。**`db_finalize` / `db_close` は失敗した
  ときだけ**更新する (後片付けが原因診断を消さない)。close 後も slot が再利用される
  までは同じ値を返す (**再利用後は新しい接続の値** — handle に世代が無いので旧利用者を
  区別しない)。範囲外 handle / 一度も開かれていない slot は `SQLITE_MISUSE`。
  `handle = -1` は呼び手 owner の直前 open 失敗。取得しても消えない。
- CPL=3 の呼び手が渡すポインタは、ディスパッチャの早期検証 (先頭番地だけ) に加えて
  wrap 側が**範囲まで**検査する: `[p, p+len)` の各ページが許可帯にあること
  (`ring3_user_range_ok`)。**PTE (present / USER) は見ない** — 許可帯の中の
  非 present なページをカーネルが写すと #PF になるが、それは既存のフォールト
  ガードが呼び手を kill する扱いで、`kprintf` の可変長 `%s` など他の KAPI と
  同じ。表を歩く実装は 2026-09-13 の実機で誤判定した (PD / アプリ PT が
  pgalloc = アプリ帯から出るので、syscall 中に物理 = 仮想で辿ると
  アプリ自身のデータを読む)。CPL=0 の直呼び (常駐シェル /
  gshell) は帯も PTE も見ない。`db_step` / `db_prepare` の 1 行が 16KB の結果ブロック
  (header + 全列 descriptor + payload) に収まらないときは、範囲外書き込みも部分 ROW も
  返さず `-1` で失敗し、`db_error_code` に `SQLITE_TOOBIG` が残る。列値の**実体化**が
  失敗した (accessor が値を返せない / `SQLITE_NOMEM`) ときも同じく `-1` で、部分 ROW を
  SHM に公開しない。

GUI 中の CPL=3 アプリは入れ子 `exec_run` を使えない (子が park できず、協調型の全体が止まる)。
そこで「外部プログラムを起動したい」と「この子を畳みたい」を**カーネルの表**に載せ、
owner 1 (WM) が top-level で取りに来て `exec_start` / `exec_kill` を実行し、結果を表へ返す。
票 [tasks/gui/v13/TASK_T9_sh.md](tasks/gui/v13/TASK_T9_sh.md) §1 D3 / §1a。実体は `exec/launch.c`。

表は **要求者 ID ごとに 1 本** (ID 2〜5 の 4 本)。欄は「配送状態」(`phase` / `kind`) と
「子の所有」(`child`) を分けてあり、取消や要求者の退場の途中でも `child` は消えない。
照合は要求者 ID ではなく **`token`** (32bit の全体単調増加、0 と負は使わない) で行う
— ID は再利用されるので、古い要求の poll / cancel が新しい住人の表に当たってしまう。

共通の規則: 出力ポインタは NULL 可 (書かない)。失敗時は出力を 1 つも書かない。
CPL=3 のポインタは既存のディスパッチャが範囲検証する。

| 名前 | 権限 | 規則 |
|---|---|---|
| `launch_req` | 宣言 `OS32X_FLAG_LAUNCHER` を持つ CPL=3 | 要求者は `res_owner_get()` で記録。cmdline は NUL 終端 1〜255B (空 / 超過 → `OS32_ERR_INVAL`)。GUI 外 (`con_sink` 無効) / 入れ子 `exec_run` の子 (`gui == 0` の非シェル) → `OS32_ERR_INVAL`。自分の表が IDLE でない (孤児回収中を含む) → `OS32_ERR_FULL`。戻り値 = token (> 0) |
| `launch_pending` | 誰でも | `PENDING` の要求数。WM は `should_park` の材料にする (0 なら何もしない) |
| `launch_take` | owner 1 | `buf` も出力なので **NULL 可** (cmdline のコピーだけ飛ばす)。`cap` を見るのは `buf` が非 NULL のときだけで、そのとき `cap < 256` → `OS32_ERR_INVAL`。要求者 ID 昇順に `PENDING` を 1 本 `TAKEN` にして token を返す (無ければ 0)。`kind` = 1 LAUNCH (buf に cmdline) / 2 KILL (`arg` = 畳む ID)。`requester` は孤児回収の表なら -1 |
| `launch_report` | owner 1 | `TAKEN` 以外 → `OS32_ERR_STALE`。LAUNCH: `rc > 0` は生きている非シェル ID でなければ `OS32_ERR_INVAL` → `child = rc`, `RUNNING`; `rc == 0` → `DONE`; `rc < 0` → `FAILED(rc)`。KILL: `rc` は無視。**取得済みの印を消して `RUNNING` に戻すだけ**で `child` は落とさない (落とすと生きている子の所有が表から消え、以後の退場でも回収されない)。`DONE` を付けるのは常に `child` の回収通知なので、正常な順序では先に付いていて `STALE` が返る — WM は再試行せず正常として扱う |
| `launch_poll` | 要求者 | `status`: `0` PENDING / `1` TAKEN / `0x100 + child` RUNNING / `0x200` DONE / `0x300 + (-rc)` FAILED。`DONE` / `FAILED` を渡した時点で表は IDLE に戻る (再 poll は `OS32_ERR_STALE`) |
| `launch_cancel` | 要求者 | `RUNNING` → `kind = KILL(child)`, `PENDING` (child は保持); `PENDING` / `TAKEN` → `OS32_ERR_AGAIN` (**これだけが「次のタイマで再試行」の合図**); `DONE` / `FAILED` / 要求者の不一致 (別 ID からの cancel、孤児回収中の表を再利用 ID が指した旧 token) → `OS32_ERR_STALE` |
| `launch_child` | 誰でも | その ID の表が所有する子 (phase を問わず)。不正 ID → 0。WM が CTRL+STOP の宛先を連鎖の末尾へ解決するのに使う |
| `sys_yield` | CPL=3 | GUI 中は **必ず** `WAIT_POLL` に park する (tick の間引きなし)。印は専用の `parked_from_yield` で、resume は**注入リングを読まず** EAX = 0 — 読むと、譲っている側が子宛の 1 バイトを吸って捨てる。park できない文脈 (CUI / CPL=0 / syscall の外 / 入れ子の子) では `hlt` 1 回して 0 |

**回収通知**: `exec_reclaim_owned(x)` の中 (`con_sink_owner_exit` と同じ位置) で、`child == x` の表を
`DONE` + `child = 0` に、`requester == x` の表を孤児回収 (`KILL(child)` の `PENDING`、`requester` は -1)
にする。子を持たない要求者の退場は表をそのまま解放する。孤児の表は誰も poll しないので、完了したら
`DONE` ではなく **IDLE** に落とす (落とさないと同じ ID の次の住人が永久に `OS32_ERR_FULL` を食う)。
通知が使うのは **ID だけ** — 正常終了は AppSlot を解放した後、`exec_kill` は前にここへ来るため。

**`exec_kill` の連鎖 (D8)**: `exec_kill(id)` は「**id とその子孫** (表の `child` を末尾まで辿ったもの) を
**末尾から** 回収」に固定した。途中の 1 本だけを畳むと残りが孤児になり、WM の `forget` と
`launch_cancel` の `DONE` も壊れる。CTRL+STOP のように 1 本だけ止めたいときは、WM が
`launch_child()` で末尾を解決してその ID を渡す (末尾は子孫を持たないので 1 本だけ畳まれる)。

### データフィールド (構造体末尾)

関数ポインタではなく値を持つフィールド。ジェネレータは `kapi-><field> = 0;` を
出力するだけなので、**実際の値は `exec_init()` / `exec_run()` で代入する**。

| Offset | フィールド | 型 | 説明 |
|--------|-----------|------|------|
| 0x3A0 | sbrk_heap_limit | `u32` | newlib _sbrk用ヒープ上限アドレス (exec_runでセットされる) |
| 0x3A4 | shm_base | `u32` | 共有メモリ (MEM_SHM_BASE) の先頭アドレス。DB結果受け渡しに使用 (exec_initでセット)。`MEM_SHM_BASE` は `__bss_end` 由来で可変なため、ユーザ空間はアドレスをハードコードしてはならない |

### §4-1 グラフィックスAPI に関する補足

v22以降、基本的な描画プリミティブ (`gfx_clear`, `gfx_pixel`, `gfx_hline`, `gfx_vline`, `gfx_line`, `gfx_rect`, `gfx_fill_rect`) は KernelAPI から**廃止**されました。

外部プログラムでグラフィックス描画を行う場合は、以下の２つの方式から選択します:

1. **libos32gfx ライブラリ** (推奨): `userland/lib/gfx/` で提供されるスタティックリンクライブラリ。サーフェス、スプライト、描画プリミティブ、ダーティ矩形管理、フォントレンダリングなど高レベルな描画機能を提供します。
2. **フレームバッファ直接操作**: `gfx_get_framebuffer()` で取得した `GFX_Framebuffer` 構造体を介して、4プレーンのバックバッファに直接書き込み、`gfx_add_dirty_rect()` + `gfx_present_dirty()` でVRAMに転送します。

**描画モード**:

| モード | 解像度 | 初期化 | ページフリップ |
|--------|--------|--------|---------------|
| 400ラインモード | 640×400 | `gfx_init()` | 自動有効 |
| 200ラインモード | 640×200 | `gfx_init_200()` | 自動有効 |

**ページフリッピング**:
`gfx_init()` / `gfx_init_200()` いずれでもページフリッピングが自動的に有効になります。
`gfx_present_dirty()` / `gfx_present_nosync()` は非表示ページにVRAM転送後、ポートA4H/A6Hでページを切り替えます。VSYNC待ちは不要となり、ティアリングが発生しません。外部プログラム側のコード変更は不要です。

### §4-2 ラスタパレット (gfx_present_raster)

v24で追加。VSYNC後のアクティブ表示期間中に、走査線ごとにパレットレジスタを書き換えることで、16色パレットの制約を超えた擬似多色表示を実現します。

- **引数**: `GFX_RasterPalTable *table` — ラスタパレットテーブルへのポインタ
- **構造体**: `GFX_RasterPalEntry` (line, pal_idx, r, g, b) × 最大200エントリ
- **動作**: dirty rectがあればVRAM転送も行い、なければパレット書き換えのみ
- **ページフリップとの併用**: フリップモードではVRAM転送をフリップ経由で行い、
  VSYNC同期のパレット書き換えのみ実行します。両モードで動作します。
- **libos32gfx ラッパー**: `gfx_raster_clear()`, `gfx_raster_add()`, `gfx_present_raster_only()`, `gfx_present_with_raster()`

### §4-3 FDリダイレクト・パイプAPI

v25で追加。外部プログラム（シェル）がFD単位の入出力リダイレクトとパイプラインを構築するためのAPI群。

**FDリダイレクト**:
- `sys_redirect_fd(fd, path, mode)` — 指定FDの出力先をファイルにリダイレクト
- `sys_reset_redirect(fd)` — リダイレクトを解除しコンソールに復帰
- `sys_is_redirected(fd)` — FDがリダイレクト中か判定
- `sys_redirect_fd_buf(fd, buf, size, len)` — FDの出力先をメモリバッファにリダイレクト
- `sys_redirect_get_buf_len(fd)` — バッファリダイレクト時の書き込み済みバイト数取得

**パイプバッファ**:
- `sys_pipe_alloc()` — パイプバッファを1個確保 (IDを返す)
- `sys_pipe_free(id)` — パイプバッファを解放
- `sys_pipe_get_buf(id)` — パイプバッファのデータポインタ取得
- `sys_pipe_get_len(id)` — パイプバッファの書き込み済みバイト数取得
- `sys_pipe_clear(id)` — パイプバッファをクリア

**典型的なパイプ実行フロー** (`cmd1 | cmd2`):
1. `sys_pipe_alloc()` でパイプ確保
2. `sys_redirect_fd_buf(1, pipe_buf, size, 0)` でcmd1のstdoutをパイプに接続
3. cmd1を実行
4. `sys_reset_redirect(1)` でstdout復帰
5. `sys_redirect_fd_buf(0, pipe_buf, len, len)` でcmd2のstdinをパイプに接続
6. cmd2を実行
7. `sys_reset_redirect(0)` → `sys_pipe_free(id)` でクリーンアップ

### §4-4 ページング問い合わせAPI

v26で追加。指定アドレスのページテーブルエントリが存在するか (Present ビット) を確認する。

- `paging_is_present(addr)` — 指定アドレスが有効にマッピングされているか判定 (1=有効, 0=Not-Present)
- **用途**: メモリダンプツール等がガードページや未マッピング領域への不正アクセスを事前に回避するために使用

### §4-5 キー押下状態ポーリングAPI

v27で追加。指定スキャンコードのキーが現在押下中かをリアルタイムに問い合わせる。
ゲームエンジン (libpyxel) のフレーム単位入力に使用。

- `kbd_is_pressed(scancode)` — 指定スキャンコードのキーが押されていれば1、離されていれば0
- IRQハンドラで128キー分のビットマップを常時更新しているため、イベントキューを消費しない

### §4-6 FM/SSG個別チャンネル制御API

v27で追加。FM音源(YM2203)の3チャンネルおよびSSG(PSG)の3チャンネルを個別に制御する低レベルAPI。
ゲームエンジンのサウンドシーケンサ実装に使用。

- `fm_note_on(ch, note)` — FMチャンネル(0-2)でノート発音
- `fm_note_off(ch)` — FMチャンネル消音
- `fm_set_tone_num(ch, tone_num)` — FMチャンネルのプリセット音色設定
- `ssg_tone(ch, period)` — SSGチャンネル(0-2)のトーン周期設定
- `ssg_volume(ch, vol)` — SSGチャンネルの音量設定(0-15)
- `ssg_all_off()` — SSG全チャンネル消音

### §4-7 マウスAPI

v28で追加。PC-98バスマウスおよびNP21/Wシームレスマウスに対応するポーリングベースのマウスAPI。

- `mouse_poll(info)` — `MouseInfo` 構造体に現在の座標・差分・ボタン状態を取得
- `mouse_available()` — マウスが使用可能か判定 (1=バスマウス, 2=シームレス, 0=なし)
- `mouse_set_bounds(x_min, y_min, x_max, y_max)` — マウス座標のクランプ範囲を設定

**MouseInfo 構造体**:
- `x`, `y` — 現在の画面座標
- `dx`, `dy` — 前回poll以降の差分
- `buttons` — ボタンビットマスク (`MOUSE_BTN_LEFT`=0x01, `MOUSE_BTN_RIGHT`=0x02, `MOUSE_BTN_MIDDLE`=0x04)
- `mode` — 動作モード (0=なし, 1=バス, 2=シームレス)

### §4-8 TVRAM読取・反転API

v28で追加。テキストVRAMの読み取りと属性操作を行う。マウスカーソル (テキストモード) の実装に使用。

- `tvram_readchar_at(x, y, *code, *attr)` — TVRAM 1セルの文字コード＋属性を読み取る
- `tvram_reverse_cell(x, y)` — 属性反転トグル (PC-98属性ビット2 (0x04) のXOR)。漢字2セル自動対応。戻り値=セル幅 (ANK=1, 漢字=2)

### §4-9 マウスカーソル制御API

v28で追加。カーネル管理のマウスカーソル表示を制御する。アプリケーションはカーソル描画を自前で行う必要がなくなる。

- `mouse_cursor_set_mode(mode)` — カーソルモード設定
  - `MOUSE_CURSOR_NONE` (0): カーソル非表示 (生ポーリング専用)
  - `MOUSE_CURSOR_TEXT` (1): TVRAM属性反転カーソル
  - `MOUSE_CURSOR_GFX` (2): GFXスプライトカーソル (将来用)
- `mouse_cursor_show()` — カーソル表示
- `mouse_cursor_hide()` — カーソル非表示 (画面更新前にhide→更新→showのパターンで使用)

---

*Last Updated: 2026-04-29*
