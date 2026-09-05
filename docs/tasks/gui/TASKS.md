# GUI シェル v1.1 — 作業分担票 (全体)

> 発行: PM (2026-09-05) / 親: [DESIGN.md](DESIGN.md) / 契約: [API_CONTRACTS.md](API_CONTRACTS.md) (2026-09-04 凍結)
> 書式は v2 の [PLAN.md](../v2/PLAN.md) / [CONTRACTS.md](../v2/CONTRACTS.md) に倣う。
> ロードマップ: `docs/ROADMAP.md` v1.1

体制は v2 と同じ 3 層 (設計 = PM、コーディング = コーダー、ビルド検証 = `os32-cycle` を回す
検証層)。レーンは 4 本で、**契約 (API_CONTRACTS.md) に対して独立に実装する**。契約を動かす
必要が出たら PM に戻す (勝手に変えると他レーンが黙って壊れる)。

---

## 1. レーンと票

| レーン | 言語 / 場所 | 票 | 内容 |
|---|---|---|---|
| **H** HAL | C, `gfx/` `drivers/` | [TASK_H1_hal_backend.md](TASK_H1_hal_backend.md) | バックエンド表 + 9801 プレーン実装 + カウンタ |
| | | [TASK_H2_pegc.md](TASK_H2_pegc.md) | PEGC 256 色バックエンド (v1.1 後半) |
| | | [TASK_H3_cirrus.md](TASK_H3_cirrus.md) | Cirrus GD54xx チップドライバ + Xe10 グルー + ai-debug screenshot 拡張 (v1.2) |
| **K** カーネル背骨 | C, `kernel/` `exec/` `sdk/kapi.json` | [TASK_K1_gui_call.md](TASK_K1_gui_call.md) | `gui_call` / `gui_register` KAPI、SHM ブロック 12〜15 予約、共有ヘッダ、エラー番号 |
| | | [TASK_K2_pump_hook.md](TASK_K2_pump_hook.md) | syscall 境界の入力ポンプ (T6) |
| | | [TASK_K3_shared_lib_band.md](TASK_K3_shared_lib_band.md) | 共有ライブラリ帯域 0x400000〜0x4FFFFF とロードアドレス移動 (最後) |
| | | [TASK_K4_gui_boot.md](TASK_K4_gui_boot.md) | `/etc/system.cfg`、テキスト GDC 制御、`os32gui` コマンド |
| **W** WM (gshell) | Rust, `userland/gshell/` (新規) | [TASK_W1_wm_core.md](TASK_W1_wm_core.md) | gshell 本体: ウィンドウ管理、スロット、イベントリング、損傷/commit、クローム、カーソル、タイマ |
| | | [TASK_W2_fep_lease_modal.md](TASK_W2_fep_lease_modal.md) | FEP を WM で持つ、14 色リースと 2 色クローム、モーダル、標準ダイアログ |
| **C** クライアント | Rust, `userland/rust/libos32gui/` | [TASK_C1_drawing.md](TASK_C1_drawing.md) | G 描画 (Rect / Style / Surface / クリップ / 文字) を libos32gfx の上に |
| | | [TASK_C2_client_loop_widgets.md](TASK_C2_client_loop_widgets.md) | `gui_call` スタブ、U3 ループ、ウィジェット木、箱レイアウト、gui_demo 書き換え |
| | | [TASK_C3_shared_lib.md](TASK_C3_shared_lib.md) | 固定アドレス・ジャンプ表・バージョン照合の共有ライブラリ化 (K3 と同時) |
| **PM** | Python, `tools/` | (票なし、本書 §5) | 共有定数の照合スクリプト、ゲート検証、`os32-cycle` の GUI 用サブコマンド |

## 2. 依存と順序

```
        H1 ─────┐
        K1 ─────┼──► W1 ──► K2 ──► W2 ──┐
        C1 ─────┤     │                  ├──► K3 + C3 (一斉再ビルド)
        K4 ─────┘     └──► C2 ─┘         │
                                         │
        H1 ──► H2 ──► H3 (screenshot 拡張は H3 の前提)
```

- **並列で始められるもの**: H1、K1、C1、K4。互いに触るファイルが重ならない (§3)。
- **W1 は K1 と H1 と K4 の後**: `gui_register` (K1) が無いと WM がアプリから呼べず、カウンタ
  (H1) が無いと性能規約 P を測れず、`sys_switch_shell` (K4) が無いと CUI から gshell を
  起動できない (契約 T9。gshell は shell.bin と同じ 0x300000 に載るので exec では起動できない)。
- **C2 は K1 の後**: op 番号と SHM レイアウトの共有ヘッダ (K1 成果物) を写す。動作確認は
  W1 と結合してから。
- **K2 は W1 の後**: ポンプの呼び先 (`gui_pump`) は W1 が作る。
- **K3 + C3 は最後**: ロードアドレス移動は os32 / os32-apps / os32-game の全 OS32X を
  一斉再ビルドして NHD を入れ替える (stale の罠)。他レーンが全部終わってから 1 回で行う。
- **H2 は H1 の後、H3 は H2 と screenshot 拡張の後**。v1.1 の完了条件には含めない
  (ROADMAP の「v1.1 後半〜v1.2」)。

## 3. 衝突ゾーンの所有権 (v2 C7 と同じ流儀)

| 所有 | ファイル |
|---|---|
| H 排他 | `gfx/**`、`include/gfx_hal.h` (新規)、`drivers/wab_*.{c,h}` (新規、H3) |
| K 排他 | `kernel/**` `exec/**` `include/memmap.h` `sdk/kapi.json` と生成物 4 つ、`sdk/include/os32/os32_kapi_shared.h`、`sdk/include/os32/os32_gui_shared.h` (新規)、`sdk/link/app.ld`、`userland/shell/**` (K4 の `os32gui`) |
| W 排他 | `userland/gshell/**` (新規) |
| C 排他 | `userland/rust/libos32gui/**`、`userland/rust/gui_demo/**`、`sdk/rust/os32api/src/gui/**` (新規、共有定数の Rust 側) |
| PM | `tools/check_gui_proto.py` (新規)、`docs/tasks/gui/**`、`docs/ROADMAP.md` |
| 共有 (PM 経由で追加) | `build/programs.mk`、`userland/deploy.yaml`、`docs/KAPI_SPEC.md`、`CLAUDE.md` |

- KAPI に足すのは K だけ。H が KAPI を要るときは K1 の票に「H1 からの依頼」として載せる
  (本書発行時点で H1 が要る KAPI は §4 の `gfx_stats` / `gfx_lease_palette` の 2 本)。
- `sdk/rust/os32api/src/kapi_generated.rs` は K の再生成物。C は触らない。

## 4. 契約から先に固める共有定数 (K1 の成果物、他レーンはこれを写す)

`sdk/include/os32/os32_gui_shared.h` に C で置き、`sdk/rust/os32api/src/gui/proto.rs` に
C レーンが同じ値を写す。PM の `tools/check_gui_proto.py` が両者を突き合わせ、`make check`
に入る。**以後は末尾追記のみ** (契約 T5)。

| 項目 | 提案値 (K1 が確定させる) |
|---|---|
| `GUI_PROTO_VERSION` | 1 |
| SHM | `MEM_SHM_GUI_BASE = MEM_SHM_BASE + 192KB`、スロット 16KB × 4、オフセット: ヘッダ 0 / 要求 16 / 応答 528 / リング 1040 (128 × 16B) / 引数 3088 (8KB) |
| op | 0 予約、1 `INIT`、2 `POLL`、3 `WAIT`、4 `COMMIT`、5 `INVALIDATE`、6 `STATS`、7 `LEASE_PALETTE`、16〜 ウィンドウ (`CREATE` `DESTROY` `MOVE` `RESIZE` `SHOW` `SET_TITLE` `CLIENT_RECT` `RAISE` `SET_FOCUS` `SET_TEXT_CURSOR`)、32〜 サーフェス (`CREATE` `DESTROY`)、48〜 タイマ (`SET` `KILL`)、64〜 モーダル (`OPEN`)、80〜 予備 |
| イベント種別 | 契約 U2 の並び順どおり 1 から: `PAINT` `CONFIGURE` `CLOSE` `FOCUS` `KEY` `TEXT` `POINTER` `BUTTON` `TIMER` `WIDGET` `MODAL` `QUIT` `PALETTE` |
| イベント配置 | **16B = 共通ヘッダ 8B (`kind` u8 @0, `sub` u8 @1, `serial` u16 @2, `window` u32 @4 = index:16 \| generation:16) + ペイロード 8B** (契約 U2 の表)。C / Rust とも大きさとオフセットを static assert |
| エラー | `OS32_ERR_STALE -11` `OS32_ERR_VERSION -12` `OS32_ERR_FULL -13` `OS32_ERR_ARG` は既存 `OS32_ERR_INVAL` (-9) を使う |
| 色 | 契約 G6 の 16 色を `GUI_COLOR_*` の役割名で、RGB は `GUI_SYSTEM_PALETTE[16]` |
| `Style.flags` | `TRANSPARENT_BG 0x01` `XOR 0x02` `DOTTED 0x04` `DITHER50 0x08` |
| 上限 | ウィンドウ 16、サーフェス 16、ウィジェット 64、リスト項目 128、タイマ 8/アプリ、クリップ深さ 8、損傷 8/ウィンドウ、文字列 256B |
| 追加 KAPI (v41) | `gui_call(u32 op, u32 arg) -> i32`、`gui_register(handler, pump) -> i32` (shell 帯からのみ)、`gfx_stats(void *out)`、`gfx_lease_palette(first, count, const u8 *rgb)`、`sys_switch_shell(const char *path) -> i32` (shell 帯からのみ、K4)、`kbd_dropped_count(void) -> u32` (カーネルのキー待ち行列が捨てた打鍵の累計。`drivers/kbd.c` の満杯分岐で加算) |

> **版番号 (v41) 確定**: ネットワークの Host Services (L3、[../network/LINK_PLAN.md](../network/LINK_PLAN.md)) も KAPI を足すが v42 にずらした。GUI (K1) が先に実装され v41、ネットワークが後発で v42 (2026-09-06 調停)。版番号割り当ての正典は [KAPI_SPEC §3-2 の予約表](../../KAPI_SPEC.md)。

## 5. ゲート (検証層が `os32-cycle` で回す。各票の完了条件はゲートに対応させる)

| ゲート | 名前 | 通過条件 | 検証 |
|---|---|---|---|
| G0 | 契約凍結 | 済 (2026-09-04)。**G0b: 契約の実装可能性** (2026-09-05 追加) = 契約末尾の 5 筋書き (イベント配置 / 重なった 2 窓 / リング満杯 / 変換中の BS / CUI ⇄ gshell) を PM が机上で通し、`check_gui_proto.py` のオフセット照合が動く | 机上 + スクリプト |
| G1 | 描ける | C1 の `gdi_test` が G API だけで契約 G6 の 16 色見本・文字・クリップを描き、H1 の `gfx_stats` が present バイト数を返す。既存 GFX テスト (gfx_demo / blit_test / hello_gfx) に回帰なし | `os32-cycle demo gdi_test` の screenshot を `docs/tasks/gui/ref/g1.png` と目視比較、`stats` を tvram で確認 |
| G2 | 窓が出る | CUI シェルから `os32gui` で gshell に切り替え (T9)、その上で書き換えた `gui_demo` (別プロセス, CPL=3) が窓 2 枚・XOR ドラッグ・フォーカス・ボタン・テキストボックス・リストボックスを動かす。重なった 2 窓で背面の再描画が前面を壊さない (G4)。1 周の syscall が `POLL` + `COMMIT` + `WAIT` の 3 回、代表操作の転送量が P2 の予算内 | `/api/key` で操作、`/api/screenshot`、`gui_bench` の tvram 出力 |
| G3 | デスクトップで起動 | `GUI=1` の `system.cfg` で起動時から gshell が上がる (K4-1)、`os32gui` / 「CUI へ」の切替が再起動なしで往復する (T9)、FEP が WM で動き (SHIFT+SPACE → 変換 → `Text` 配送)、14 色リース中に 2 色クロームが出る、モーダルが入れ子ループ無しで閉じる、計算ループ中のアプリ (KAPI は呼ぶ) でもカーソルが追従しクリックが失われない (K2、T8 の限界どおりメニューは開かない) | `os32-cycle deploy` からの起動確認 + G2 と同じ操作 + FEP 手順 (メモリ os32-fep-testing) |
| G4 | 共有ライブラリ | ライブラリが 0x400000 に常駐、アプリが 0x500000 から動き、os32-apps / os32-game が再ビルドで無変更動作。`gui_demo` の .bin がライブラリ分だけ小さくなる | `heap_test` の guard_a 表示、`make external` + 全 deploy.yaml の起動確認 |
| G5 | 9821 (v1.1 後半〜) | PEGC 256 色で G1〜G3 と同じ絵、Cirrus Xe10 で HW 塗り / BLT の能力ビットが立ちカウンタの hw_ops が増える | np21x64w.ini の切替 ([D2] 承認要) + screenshot 拡張 |

- 「配備完了」の文言だけを信じない ([V1]、メモリ os32-nhd-deploy-lock)。ゲートは必ず NHD
  配備 + 再起動後の実機到達で判定する。
- **性能は NP21/W で測れない** (DESIGN §8)。ゲートの数値判定はカウンタ (回数) で行う。

## 6. 全レーン共通の鉄則

1. **契約を動かさない。** 足りないものは PM へ。op / イベント / 構造体は末尾追記のみ。
2. **ポインタを SHM とイベントに載せない** (契約 T1)。文字列は長さ前置で値渡し。
3. **決め打ち禁止**: 640×400 / 16 色 / プレーンを GUI と WM に書かない。`gfx_screen_info()`
   を信じる (契約 G5)。H 以外がポート番号や VRAM アドレスを書いたら差し戻し。
4. **Rust は no_std、外部クレート無し、描画は libos32gfx を FFI** (v2 C8 をそのまま継承)。
5. **C89、`kstr*`、三層定数** ([C1] [C2] [C4])。ポート・ID は資料の出典をコメントに
   (`docs/hw/undocumented/io_*.md`。ミラーは git 管理外)。
6. **検証は [V1]〜[V4]**: deploy.yaml に載せる、NHD 配備で確かめる、失敗はそのまま書く。
7. **`get_tick` スピン禁止** (v2 M0)。待つのは `OP_WAIT` → `sys_halt` だけ。

## 7. 進捗 (PM が更新。ゲートは実機 = NP21/W の NHD 配備で判定)

| 日付 | 事項 |
|---|---|
| 2026-09-05 | K1 (KAPI v41 / 共有ヘッダ)、H1 (GfxBackend 表 / backend_pc98)、C1 (G 描画 API / gdi_test)、K4 (system.cfg / CUI⇄gshell 起動ループ / os32gui) を feat/gui に結合。**G1 通過** (gdi_test が描画し `gfx_stats` の present_bytes=512000)。レビュー 2 回 (11 点) 反映 |
| 2026-09-06 | **W1 結合** (gshell: handler / pump / cursor / desktop / wm / slot / ring / damage / visible / chrome / input / timer、`make gshell` で 0x300000 リンク、`/bin/gshell.bin`)。カーネル修正 §4-19 (CPL=3 の KAPI 中に IF=0 で hlt が永久停止 → `int80_stub` で sti / 出口 cli。G2 以降の `OP_WAIT` の前提)。**G2 の W 側 (単独)**: `os32gui` → デスクトップ (背景・手引き・カーソル) → ESC → CUI、3 往復で `ver` 応答。残りの G2 (gui_demo の窓 2 枚、ドラッグ、G4 の重なり、契約 P) は C2 結合後 |

| 2026-09-06 | **K2 結合** (syscall 境界ポンプ、CTRL+STOP で CPL=3 を畳む経路を新設、GUI 中は cooked に積まない)。**C2 結合** (libos32gui: client / app (U3) / window / widget / layout / timer、gui_demo 書き換え、lease_test、gui_bench)。**レビュー #3 (6 点) 反映**: ① 可視領域の打ち切りは部分集合のみ、② raw キーにイベント時点の修飾 (`keycode \| down<<8 \| mods<<9`)、③ raw は GUI 中のみ + 切替時に破棄、④ 切替要求は `gui_take_next_shell` の consume 方式、⑤ タイマ ABI を契約 U5 に (`u8 id / u8 repeat / u16 interval_ticks`)、⑥ フォーカス = 最前面の可視窓 (hide / create で Focus を流す)。実機検証: `make all` → NHD 配備 → kselftest 42/0 → gdi_test 復帰 → `os32gui` → **F1 で gui_demo の窓 2 枚 (Widgets / Help) が出て TAB でフォーカスが巡回、ESC でアプリの窓が全部消え (owner 回収)、ESC で CUI へ、ver 応答** = **G2 の主要項目通過** (screenshot: tools/emu_agent/logs/20260906-034431/shots/step12・16・19)。未確認: XOR ドラッグと重なりの再描画 (G4) は **NP21/W ai-debug にマウス注入 API が無い** (`/api/key` のみ) ため自動化できず、`/api/mouse` の追加 (np21w-src 側) が要る。契約 P の syscall 回数は gui_bench で次回 |

| 2026-09-06 | **W2 結合** (FEP を WM で: SHIFT+SPACE / 未確定行と候補窓の GFX 描画 / Text 配送、14 色リースと 2 色クローム、モーダルと標準ダイアログ、F1〜F5 起動)。K 依頼 2 本を PM が実装: **`ime_feed_key` / `ime_set_render` を KAPI v41 末尾に追記** (180 関数。v41 は main 未リリースのため同じ版、v42 のネットワーク予約は不変)。共有ヘッダに `GUI_MODAL_*` を追記。未定義: ファイル選択のパスをアプリへ返す `GUI_OP_MODAL_RESULT` (追記候補)。K3 / C3 (G4) を投下中 |

| 2026-09-06 | **G3 通過 (実機)**: 一斉再ビルド (`make clean` → `make all`) → NHD 配備 → kselftest 42/0。(1) FEP: gui_demo のテキストボックスで SHIFT+SPACE → `nihongo` → SPACE → RETURN で「日本語」が入る (`ime_feed_key` / GFX 版 `IME_Render` 経由)。(2) リース: lease_test (F4) で 14 色が独自パレット、クロームが白黒 2 色・デスクトップが市松、ESC で復元。(3) ポンプ: gui_busy (F3) の計算中に 20 打鍵 → `POLL=68 (key=40 text=20 ptr=5)`、`flags=0 dropped=0 kbd_dropped_delta=0` (偽 OVERFLOW 無し)。(4) モーダル: F5 のファイル選択が開閉。(5) `os32gui on` → `system.cfg GUI=1` → リセットで gshell が起動時に上がる → ESC で CUI → `os32gui off`。screenshot は scratchpad の `g3_*.png` (セッション内)。**残課題**: FEP の `[あ]`/候補窓が左下固定 (テキストカーソル位置に出ない — C2 の textbox が `SET_TEXT_CURSOR` を送っているか要確認)、`ime_toggle` が TVRAM に "Dict loaded" 等を出す (GUI 中は抑止したい)、gui_busy の窓は WM の X3 が回らないので枠が出ない (契約 T8 の限界として記録)、`--nokapi` + CTRL+STOP は起動手段 (引数付き起動) が無く未検証、gshell 常駐中は hotdeploy が効かない (K2 の注記) |

### 記録しておく v1 の限界 (W1 の申し送り)

- **commit 前の描画が表示面に出うる**: ソフトウェアバックエンド (9801) は単一バックバッファを WM とアプリで共有するので、WM の X3 present (クローム / デスクトップ) がアプリの未 commit 描画を巻き込む。契約 G4 は「バックエンドの責任」としており、v1 では許容。PEGC / Cirrus (H2 / H3) でサーフェスを分けられれば解消。
- **cooked キュー**: WM は raw リングだけ読むので、GUI 中に cooked (`kbd_buf`) が溢れて `kbd_dropped` が偽 OVERFLOW になる。暫定は gshell の `drain_cooked_queue`、本筋は K2 で「GUI 中は cooked に積まない」。
- W1 の解釈 (契約に受け渡し場所の記述が無かったもの): `OP_INIT` の proto_version は arg、単一ハンドル op は arg≠0 ならハンドル / 0 なら `GuiReqWindow`、`OP_STATS` は応答ブロック先頭に `GFX_Stats` 生、P2 の取り込み tick はスロット +11280 に 64×4B (`u16 serial` + `u16 tick`)、`Configure` の rect は画面絶対座標 / `Paint` はクライアントローカル。タイマは契約 U5 のとおり (レビュー #3 ⑤で修正、反復固定は撤回)。C2 はこれに合わせてある。`Key` は down=false (break) も届く (契約 U2a の「当面届かない」より前進、クライアントは編集に使わない)。gshell 配下のアプリは `libos32gfx_init()` を呼ばず `client::init()` の attach を使う (gfx_init が VRAM とパレットを壊す)。
