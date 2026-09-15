# OS32 リリースロードマップ

*策定: 2026-04-17 / v1.x GUIシェル計画 / 2026-09-15 更新 (v1.3 完了、Host Services N1〜N4 受入完了、移植準備 1〜4 着地)*

## 0. 版数の対応表 (正典はここだけ)

版数の線は **2 本**あり、番号が重なるので混同しない。

| 線 | 現在 | 意味 | 記録 |
|---|---|---|---|
| **カーネル** | **2.0** (タグ `v2.0`、2026-09-03) | リング 3 (CPL=3) ネイティブ。`ver` が `OS32 v2.0` と名乗るのはこれ | [archive/kernel_v2/PLAN.md](archive/kernel_v2/PLAN.md) (M1〜M3 の**完了記録**)、[CHANGELOG.md](../CHANGELOG.md) |
| **GUI シェル** | **1.3 完了 → 1.4 進行中** | 本書 §1 の各節。カーネル 2.0 の上で動く | §1 |
| 次期カーネル | **v3 (未着手、未定義)** | 本書 §2 の長期項目 (プリエンプティブ寄りのマルチタスクなど)。**2026-09-15 のユーザー決裁で「v2」ではなく v3 と呼ぶ** (出荷済み 2.0 と衝突するため) | §2 |
| ゲーム基盤 | v4 (草案) | [V4_GAME_PLATFORM_DRAFT.md](V4_GAME_PLATFORM_DRAFT.md)。v3 の後 | — |

*v1.0 到達までの開発履歴は [archive/ROADMAP_v1.0.md](archive/ROADMAP_v1.0.md) を参照*

---

## 1. v1.x GUIシェル — 「デスクトップ革命」

**コンセプト**: Windows 3.1 / 早期 Windows 95 の**見た目**をモデルにしたグラフィカルデスクトップ環境。OS32のCLI・グラフィックス・ファイルシステム全技術の集大成。

API は Win16 の再現ではなく、その欠点を 386 で払える範囲の現代の様式で解消する。
設計記録: [tasks/gui/DESIGN.md](tasks/gui/DESIGN.md) / v1.1 凍結契約: [tasks/gui/API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md)。

### 設計方針

| 項目 | 決定 |
|------|------|
| デザインモデル | **Win3.1 / 早期Win95 の外観** — 協調型シングルタスクGUIデスクトップ (WM は gshell 常駐) |
| 描画方式 | **全面GFX描画**。9801 = 640×400×16色 planar、9821 = PEGC 640×480×256色 / Cirrus GD54xx を HAL で切替 |
| 再描画モデル | InvalidateRect 方式 (damage + commit)、window move は XOR 枠、全画面 backbuffer + clip |
| GUI API | **libos32gui** — 非同期・ID参照・型付き16B event + retained widget tree + stateless drawing + box layout |
| app ⇄ WM | KAPI `gui_call(op,arg)` 1 本 + GUI SHM slot 4 本。wire protocol に pointer を載せない |
| GUI library | shared library band 0x400000〜0x4FFFFF、app は 0x500000 から |
| 色 | system 16色 + focused app の 14色 lease。lease 中 WM chrome は2色 |
| CUI/GUI | `/etc/system.cfg` の GUI=0/1 は**次回 boot の既定値**。実行中 shell の切替は `sys_switch_shell` |
| FEP | gshell が GFX renderer を保持。CUI へ戻る前に renderer callback を解除 |
| 性能目標 | Pentium 100MHz / 32MB で「超快適」を目標とするが、32MB をメモリの設計上限にしない |
| メモリ方針 | CUI 最低 8MB は GUI 要件・開発制約ではない。GUI 必要 RAM は実測で定義。32bit フラット空間の設計対象と現行実装上限は [02_memory.md](02_memory.md) を参照 |

### 技術基盤

| 既存資産 | GUIシェルでの活用 |
|---------|-----------------|
| libos32gfx | client drawing の下敷き。planar / packed8 の差を吸収 |
| KAPI `gfx_screen_info` / `gfx_hw_*` | backend capability / hardware fill / blit |
| KCG cache | ANK + 漢字の GFX text |
| mouse sprite | GUI cursor |
| `ime_render` | FEP 未確定文字列・候補窓を WM が描画 |
| `sys_halt` | `OP_WAIT` / gshell idle の待ち。**1回の hlt であり system shutdown ではない** |
| Ring3 + address-space isolation | app と display window / shlib / guard の保護 |
| shared lib loader | libos32gui.shlib の text共有 + data app別複製 |

### CUI / GUI 切替フロー

```text
[CUI -> GUI: 即時]
1. CUI shell で `os32gui`
2. shell: sys_switch_shell("/bin/gshell.bin")
3. shell exit
4. kernel shell loop が gshell を 0x300000 へロード

[次回 boot の既定値]
- `os32gui on`  -> system.cfg GUI=1
- `os32gui off` -> system.cfg GUI=0

[GUI -> CUI: v1.2]
1. Start -> CUI mode
2. running app があれば Quit(SWITCH_CUI)
3. app exit
4. gshell: cursor hide / ime_set_render(NULL) / gfx_shutdown
5. system.cfg GUI=0 を保存
6. sys_switch_shell("/sys/shell.bin")
7. gshell exit -> CUI shell

[Shut Down: v1.2]
1. Start -> Shut Down
2. running app があれば Quit(SHUTDOWN)
3. app exit
4. gshell: GFX/FEP cleanup
5. halt screen
6. for (;;) sys_halt()
※ v1.2 は電源OFFではなく system halt。reset で再起動する

[ハング復旧]
1. FDDから boot -> mount /hd0
2. /hd0/etc/system.cfg を GUI=0 に変更
3. reboot -> CUI
```

---

### v1.1 — 「GUI基盤」 ✅ 完了

**ゴール**: mouse / window / event / drawing / FEP / modal / 3 backend を含む GUI 基盤を完成する。

作業分担・検証履歴: [tasks/gui/TASKS.md](tasks/gui/TASKS.md)。

> **2026-09-06: main にマージ (`8e184e2`)**。G1〜G5 を NP21/W で通過、レビュー 6 回反映、KAPI v42。
> 9801 planar / PEGC / Cirrus Xe10 の3 backend、Ring3 display isolation、shared lib、FEP、palette lease、modal を実機確認。
> 同日、ai-debug `/api/mouse` を追加し drag / overlap / click delivery を自動検証可能にした。

#### v1.1 の主要成果

| 項目 | 状態 |
|------|------|
| `gui_call` + GUI SHM + event ring | ✅ |
| syscall boundary input pump / CTRL+STOP Ring3 abort | ✅ |
| gshell WM / Z-order / focus / damage / chrome / cursor / timer | ✅ |
| libos32gui retained widgets / U3 loop | ✅ |
| fixed-address `libos32gui.shlib` | ✅ |
| FEP GFX renderer | ✅ |
| 14色 palette lease / 2色 chrome | ✅ |
| modal / MessageBox / basic File Open UI | ✅ |
| PC-9801 planar 640×400 | ✅ |
| PEGC 640×480 packed8 | ✅ |
| Cirrus Xe10 640×480 + hardware ops | ✅ |
| `/api/mouse` / `/api/screenshot` regression support | ✅ |

> 初期計画では H3 (Cirrus) を「v1.1後半〜v1.2」としていたが、実際には v1.1 の G5 までに完了した。v1.2 では HAL は原則 freeze / regression のみ。

---

### v1.2 — 「デスクトップ環境」

> **状況 (2026-09-07)**: main へマージ済み (`d739494`)。G0〜G5 の検証記録と既知の検証上の制約は [tasks/gui/v12/TASKS.md](tasks/gui/v12/TASKS.md) §10 を参照。ESC 即時切替と上部バーは撤去済み (`DEBUG_SHORTCUTS`)。

**ゴール**: taskbar・Start・File Manager・launcher が揃い、GUIだけで基本操作が完結する。app 置換、CUI 切替、system halt を現在の single-foreground-app model を壊さず実現する。

正式設計:

- [tasks/gui/v12/CONTRACTS.md](tasks/gui/v12/CONTRACTS.md)
- [tasks/gui/v12/TASKS.md](tasks/gui/v12/TASKS.md)

**KAPI v42 は維持。v43 は network / Host Services 用予約。** GUI wire protocol と shlib jump table の末尾追記で進める。

| 作業 | カテゴリ | 備考 |
|------|---------|------|
| Taskbar | WM | 画面下部24px、Start・window button・clock。app window/SHM slotを消費しない |
| Start menu | WM | Programs / File Manager / Run / CUI mode / Shut Down |
| Session state machine | WM/API | `SESSION_REQUEST`、sticky Quit。nested `exec_run()` 禁止 |
| CUI switch | WM | app終了後、GUI=0保存 + `sys_switch_shell`。不要な reboot はしない |
| System halt | WM | app終了・GFX/FEP cleanup 後 `for (;;) sys_halt()`。電源OFFは対象外 |
| Standard dialogs | WM/API | MessageBox / File Open / Input。completed result を event ring と独立保持 |
| Modal result | GUI protocol | op 65。wrong/double consume = STALE、未consume次modal = FULL |
| File Manager | app | Win3.1風2 pane、navigate / mkdir / rename / delete / copy / same-FS move / launch |
| App launch request | GUI protocol | op 66。filer は `exec_run()` を直接呼ばず gshell へ LAUNCH(path) を依頼 |
| Icon16 | API | **16×16固定**、4bpp + 1bpp mask。32×32 / PNG/BMP/ICO は後段 |
| Right-click menu | WM/app | popup Window ABI は作らず WM overlay / client-area overlay |
| Regression automation | Tool | **既存 `/api/mouse`** + `/api/key` + screenshot/status を利用 |

#### v1.2 の重要な制約

- external GUI app は同時に1本。
- app 実行中の WM は app syscall 文脈で動くという v1.1 T8 を維持。
- X4 で VFS / exec / cfg 更新をしない。
- Quit / Modal completion は control event として ring full で捨てない。
- file copy は event loop に定期的に戻る。
- PC98 / PEGC / Cirrus の3 backend で同一 desktop flow を通す。

#### v1.2 Gates

| Gate | 内容 |
|---|---|
| G0 | protocol / 個別票 freeze、KAPI v42維持 |
| G1 | taskbar / Start / clock / focus |
| G2 | modal result / Input / FEP / stale組合せ |
| G3 | File Manager 基本操作 |
| G4 | app置換 / CUI / halt / CTRL+STOP後pending action |
| G5 | 3 backend + v1.1 regression + static gates |

---

### v1.3 — 「ターミナル統合とCUI抽象化」 ✅ 完了 (2026-09-14)

全項目受入済み・main にマージ済み (`fac0d89`)。残件の小物 4 件 (タスクバー経路の試験、`stat`、S6 `tar`、試験の棚卸し文書) も 2026-09-14 に feat/gui へ着地。持ち越し: S6-P (ext2 の小書き込み性能、[tasks/settings/TASK_S6.md](tasks/settings/TASK_S6.md))、F3a〜c 等の保留 5 件 (ユーザーの再考待ち、[tasks/agents/HANDOVER_v14.md](tasks/agents/HANDOVER_v14.md) §3)。

着手計画: [tasks/gui/v13/PLAN.md](tasks/gui/v13/PLAN.md)、監査と決裁: [AUDIT_2026-09-10](tasks/gui/v13/AUDIT_2026-09-10.md)。
2026-09-10 決裁: **GUI アプリ 4 本の同時実行 (契約 T2a) を v1.3 の最初に置く** ([K5](tasks/gui/v13/TASK_K5_multiapp.md))。
端末は外部アプリ。hermes 期の T5b (常駐パネル) は撤去、T6a (有限実行) は破棄。

**ゴール**: GUI desktop 上で CUI command が実行でき、既存 CUI program との互換性を確保する。

**目安: v1.2 から 2〜3ヶ月**

| 作業 | カテゴリ | 備考 |
|------|---------|------|
| **GUI アプリ 4 本の同時実行** | kernel / GUI | 契約 T2/T2a。PD 切替は `OP_WAIT` の中だけ、5 本目は `ERR_FULL`、資源回収はアプリ単位。[K5](tasks/gui/v13/TASK_K5_multiapp.md)。端末と CUI 子の同居の土台 |
| ターミナルウィンドウ | app | **外部アプリ**。libos32term (セルモデル) + libos32term_render (厳密 clip) を Paint に接続 |
| CUI program output redirect | kernel | console_write -> terminal window virtual console |
| full-screen GFX program | GUI | exec_run後にGUI全体再描画 |
| shell script | terminal | terminal内 script engine |
| CUI/GUI abstraction | API | console/GUI window の I/O 抽象化 |
| 設定レジストリ | system/API | `/etc/settings.db` (SQLite) + `libos32cfg`、初期値はインストール媒体のみ (リカバリモードで復元)。設計: [tasks/settings/DESIGN.md](tasks/settings/DESIGN.md)。v1.4 の設定アプリの下敷き |

#### CUI/GUI 抽象化レイヤー

```text
[CUI]                          [GUI]
 app                            app
  ↓                              ↓
 KernelAPI                     KernelAPI
  ↓                              ↓
 console.c -> TVRAM            console.c -> gshell terminal
```

- 既存 `shell_print` / `console_write` を GUI mode では terminal window へ redirect。
- `kbd_getchar` / `kbd_getkey` を GUI event queue と統合。
- GUI native app は libos32gui を直接使用。

---

### v1.4 — 「ホストサービスと最小のアプリ」

**ゴール**: Host Services (LGY-98 経由の GET / 印刷 / クリップボード、[tasks/network/HOST_SERVICES_PLAN.md](tasks/network/HOST_SERVICES_PLAN.md)) を
コマンドと GUI から使えるようにし、GUI アプリは **About とテキストエディタの 2 本だけ**に絞る
(ユーザー決裁 2026-09-14: アプリ群は葉なので後回し、エディタは libos32gui / 設定 / ホストサービスを
通しで使う受入試験として 1 本残す)。

**目安: v1.3 から 3〜6ヶ月**

| 作業 | カテゴリ | 担当 (ROLES §0) | 備考 |
|------|---------|------|------|
| Host Services N1〜N4 (**受入完了 2026-09-15**) | kernel / host / command / shlib | Claude Code PM + Opus 5 コーダー | ワイヤ v2、KAPI v51、`host_agent.py` v2、`wget` / `lpr` / `hclip` / `date -sync`、`host_*` ラッパー |
| PEGC / Cirrus の 8bpp バックエンド (R2) | GUI | 基盤 | S5 から先送り |
| N4 のアプリ側 (ファイラの印刷、端末のコピー / 貼り付け) (**受入完了 2026-09-15**) | app | Claude Code PM + Opus 5 コーダー (別エージェント案は 2026-09-14 に撤回) | libos32gui の `host_*` ラッパー経由 |
| About dialog | app | アプリ層 | OS32 About |
| text editor GUI | app | Claude Code PM + Opus 5 コーダー | edit.bin GUI版。**API の退行検出を兼ねる** (N3 の後に着手) |
| Host Services N5 (実機 LAN、Npcap + scapy) | host / driver | — | **保留** (エミュレータで N1〜N4 完了、実機は未着手) |
| hsync H1 / H3 (同サイズ差し替えの検出、日時前置判定、KAPI v52) | system / fs | Claude Code PM + Opus 5 コーダー | **受入完了 2026-09-15**。H2 (置換の安全化) / H4 (配備マニフェスト) は未着手 |
| ext2 の B8 (読み取り失敗の読み替えを塞ぐ、remount-ro 相当) | fs / vfs | 同上 | **受入完了 2026-09-15** ([tasks/shell/TASK_FS_TYPE.md](tasks/shell/TASK_FS_TYPE.md)) |

先送り (v3 以降、[§2](#2-長期ロードマップ-次期カーネル-v3-以降) の「GUI アプリケーション群」): 設定アプリの拡張項目、
image viewer (VBZ / VDP / BMP)、music player、`sed` / `awk`。

---

## 2. 長期ロードマップ (次期カーネル v3 以降)

### 協調型マルチタスク

**協調型の複数アプリ (最大 4 本、PD 切替、譲り合いは `OP_WAIT` だけ) は契約 T2a のとおり v1.3 で実装する**
(2026-09-10 決裁。以前ここに「v1.x は single foreground app」とあったのは v1.2 の暫定を指していた)。
v3 では timer interrupt を利用したプリエンプティブ寄りの multi-task を検討する。

- window / process の独立実行
- v1.x `gui_call` + SHM event ring を拡張した IPC
- child program 実行中も desktop が独立して応答

### 他アーキテクチャへの移植に備えた調査 (継続)

移植 (例: ARM) は v1.x の範囲外だが、**新しい層を実装するたびに CPU 依存の調査を票に含める**
(ユーザー指示 2026-09-14)。最初は Host Services N1 (ワイヤ v2 / `link.c` / KAPI v51) で
`docs/tasks/portability/SURVEY_N1.md` に記す (観点は `docs/archive/network/TASK_N1.md` §0 段 7)。
以後の票も同じ観点で `docs/tasks/portability/` に追記する。**移植準備の 4 段は 2026-09-15 に着地した**
(ARM コンパイル計測 `make check-arm-compile` 55/93、`hlt`/`cli`/`sti` を `io.h` 経由に、`arch/x86` + `platform/pc98`
の骨格、kstring の C 版、LE アクセサ `include/endian_le.h`。基準値と経過は [tasks/portability/ARM_GAUGE.md](tasks/portability/ARM_GAUGE.md))。
残りは `gdt`/`tss`/`cr3` と CPL=3 降下 asm の `arch/x86/` への移設、ARM 実装、KAPI 生成器の arch 対応 (**v3 まで保留**、ユーザー決裁)。
習慣として今から守るもの: ワイヤ / ディスク上の構造は LE アクセサで読む、非アラインアクセスをしない、
絶対番地は `memmap.h` 以外に書かない、割込み制御は既存ヘルパー経由。

### GUI アプリケーション群 (v1.4 から先送り、2026-09-14)

v1.4 の「アプリ群」は基盤に依存される側ではないので、協調型マルチタスクの拡張の後に回す。
設定アプリの拡張 (壁紙・色・マウス速度は設定レジストリに行を足すだけ、UI は gshell の設定ダイアログ)、
image viewer (MGX は `mgxview` が既にある。VBZ / VDP / BMP を足す)、music player (FM 音源 BGM)、
`sed` / `awk`。着手の順は、そのときに一番 API の穴を踏みそうなものから。

### 16bit DOSプログラム移植スキーム

```text
[DOS 16bit .COM/.EXE]
        ↓
 reverse engineering / static analysis
        ↓
 INT 21h -> KernelAPI mapping
        ↓
 32bit OS32X
```

- 半自動 + 手動修正を想定。
- 小さな COM tool から case study。
- INT 21h -> KernelAPI compatibility layer が中心課題。

### その他の長期テーマ

- PC-98 NIC (C-bus LAN) / network: [tasks/network/PLAN.md](tasks/network/PLAN.md)

---

*ホビープロジェクトとして品質優先で進行。タイムラインはデッドラインではなくペース感の目安。*
