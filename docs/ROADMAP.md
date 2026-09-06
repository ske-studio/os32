# OS32 リリースロードマップ

*策定: 2026-04-17 / v1.x GUIシェル計画 / 2026-09-06 改訂 (v1.1 完了・v1.2 契約反映)*

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
| 性能上限 | Pentium 100MHz / 32MB で「超快適」を目標。それ以上の機能は対象外 |

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

### v1.3 — 「ターミナル統合とCUI抽象化」

**ゴール**: GUI desktop 上で CUI command が実行でき、既存 CUI program との互換性を確保する。

**目安: v1.2 から 2〜3ヶ月**

| 作業 | カテゴリ | 備考 |
|------|---------|------|
| ターミナルウィンドウ | app | lconsole統合、KCG fontでGVRAM描画 |
| CUI program output redirect | kernel | console_write -> terminal window virtual console |
| full-screen GFX program | GUI | exec_run後にGUI全体再描画 |
| shell script | terminal | terminal内 script engine |
| CUI/GUI abstraction | API | console/GUI window の I/O 抽象化 |

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

### v1.4 — 「GUIアプリケーション群」

**ゴール**: GUI専用 application が揃い、v2.0 に向けた基盤が完成。

**目安: v1.3 から 3〜6ヶ月**

| 作業 | カテゴリ | 備考 |
|------|---------|------|
| 設定 application | app | wallpaper・color・mouse speed 等 |
| text editor GUI | app | edit.bin GUI版 |
| image viewer | app | VBZ/VDP/BMP |
| music player | app | FM音源 BGM control |
| About dialog | GUI | OS32 About |
| `sed` / `awk` 等 | command | v1.0から先送り |

---

## 2. 長期ロードマップ (v2.0+)

### 協調型マルチタスク

v1.x は single foreground app だが、v2.0 では timer interrupt を利用した協調型 multi-task を検討する。

- window / process の独立実行
- v1.x `gui_call` + SHM event ring を拡張した IPC
- child program 実行中も desktop が独立して応答

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
