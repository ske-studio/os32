# CLAUDE.md — AI コーディングアシスタント向けガイダンス

AI コーディングアシスタント共通の入口。**置くのは指示と参照だけ**で、
番地・KAPI 表・ファイル地図・障害の経緯といった技術情報の本文は置かない。更新先は
[`docs/INDEX.md`](docs/INDEX.md) 冒頭の「情報単位ごとの正典」表が 1 か所に決めている。

## 体制

PM = Claude Code (`claude-fable-5-1`)、コーダー = サブエージェント (`claude-opus-5`, worktree 隔離)、
レビュアー = **Codex** (`codex exec -s read-only`。2026-09-14 に Fable 枯渇で復帰、範囲を絞って依頼)、テスター = ローカル AI (`tools/emu_agent/`、スキル `os32-local-ai`)。
実装は基盤・アプリ層とも Claude Code (PM) が Opus 5 サブエージェントで行う (2026-09-14 更新、別エージェント案は撤回。ROLES §0)。
役割の境界・起動コマンド・規約の正典は [`docs/tasks/agents/ROLES.md`](docs/tasks/agents/ROLES.md)。
**カーネル層 (カーネル本体・VFS/FS・exec/ページング・KAPI・shlib 読み込み) に分かっている不具合が
あるあいだは、新機能より先に直す** — 理由と適用の仕方は
[`docs/POLICY_DEV.md`](docs/POLICY_DEV.md) §1。
承認済みスコープの中では止まらずに進め、止まるのは [D1]〜[D3] の承認・仕様の分岐・スコープ拡大・
**独立レビューが要る地点** の 4 つだけ。レビューは PM が代行せず、ROLES §5 の書式で報告して渡す。

## Project Overview

OS32 is a 32-bit bare-metal OS for NEC PC-9801/9821 machines, built with an i386-elf GCC
cross-compiler and NASM. The kernel runs in protected mode at physical 0x100000; external
programs load at 0x500000 and run at CPL=3 in their own page directory.

## Build Commands

Full target list and compiler flags: [`docs/08_build.md`](docs/08_build.md#ビルドターゲット) §8-4 / §8-2.
Which build and which verification a change actually needs: skill **`os32-build-verify`**.

```bash
make all / kernel / programs              # build (SDK and images come with `all`)
make external                             # apps/ + game/ — after any KAPI or SDK library change
make check                                # KAPI version, manifests, constraint IDs, GUI proto, etc.
make clean                                # required after a KAPI struct change ([ABI3])
make deploy                               # HostDrv (C:\os32) — no reboot, not verification ([V1])
# userland delivery: make deploy (host -> C:\os32) then `hsync` in the guest
#   hsync skips /sys (running shell + shlibs) unless you name it: `hsync sys`
make deploy-kernel / deploy-boot          # NHD / boot area — stop NP21/W first ([D1])
```

Which of the three deploy paths a change needs: [`docs/08_build.md`](docs/08_build.md#配備3経路) §8-4.

### Talking to the guest (NP21/W)

The debug HTTP server is built into `np21x64w.exe` (the ai-debug fork), enabled with `aidebug=true` /
`aidbport=8025` in `np21x64w.ini` — no relay process. Endpoints `/api/cmd` `/api/key` `/api/mouse`
`/api/tvram` `/api/screenshot`; the curl recipes, the `ax/ay` formula and the URL-encoding trap are in
[`docs/POLICY_DEBUG.md`](docs/POLICY_DEBUG.md) §5. Registers, memory, disassembly, breakpoints and
tracing go through `tools/np21w_mcp/`. Chasing a failure on the emulator: skill **`os32-emu-debug`**.

## Project Constraints

規則の正典は [`docs/CONSTRAINTS.md`](docs/CONSTRAINTS.md) — 理由と詳細はそこにある。ここは規則行だけで、
ずれは `make check` (`tools/check_constraints.py`) が ID で検出する。
[D1]〜[D3] の一部の操作には `.claude/settings.json` の `ask` / `deny` も設定している。
これは包括的な保護ではない。NP21/W の停止確認や別コマンド経由の操作にも正典の規則を適用する。

- **[C1]** C89 (GNU89) only — no `//` comments, declarations at block start, no C99 features.
- **[C2]** In the kernel use `kstrncpy` / `kstrncat` / `kstrlen` / `kstrcmp` (`lib/kstring.h`), never libc.
- **[C3]** Functions exposed to external programs need `__cdecl` wrappers in `kapi/`.
- **[C4]** No hardcoded constants — follow the three-layer constant scheme.
- **[HW1]** Never use EGC / GRCG / GDC drawing commands. The CPU writes straight to VRAM at `0xA8000`.
- **[HW2]** DMA buffers must not straddle a 64KB boundary.
- **[ABI1]** `sdk/kapi.json` is the single source of truth. Never hand-edit the generated files.
- **[ABI2]** Append KAPI entries only. Never reorder or delete an existing slot.
- **[ABI3]** After a KAPI change, bump the version and run `make clean` → `make all`.
- **[V1]** `make deploy` (HostDrv) alone is not verification — PATH prefers the NHD's `/usr/bin`.
- **[V2]** Register every launchable binary in its own layer's `deploy.yaml`.
- **[V3]** Do not shorten the curl timeout for remote execution (15s minimum, 60s+ for long runs).
- **[V4]** Report failures and skipped steps as they happened. Never present something unverified as a pass.
- **[D1]** Never deploy to the NHD while NP21/W is running. Stop → deploy → start.
- **[D2]** Get approval before irreversible operations (overwriting the NHD or image masters,
  `rm -rf`, `git reset --hard`, editing `*.ini`).
- **[D3]** Never put the contents of `.env`, API keys or passwords in output.

## Architecture

**Memory layout** — definitions in `include/memmap.h`, the explanation in
[`docs/02_memory.md`](docs/02_memory.md) §2-1. Bands only:

| Band | Contents |
|---|---|
| `0x00000–0x9FFFF` | Conventional — font cache, Unicode table (0x4A000), GFX backbuffer (0x6A000), hot-deploy block (0x8C000), autoplay mailbox (0x90000). Handed whole to the V86 guest |
| `0xA0000–0xFFFFF` | VRAM (text + graphics planes) and BIOS ROM |
| `0x100000–0x2FFFFF` | Kernel (binary + heap + KAPI + 224KB SHM), then SQLite from 0x200000; the 16KB kernel stack sits at the **top of the SQLite band** with its guard below it. **The detailed map is generated — `docs/02_memory.md` §2-1 is the only copy** (`tools/gen_memmap.py`, checked by `make check`) |
| `0x300000–0x4FFFFF` | Resident shell (two heaps: newlib sbrk, exec_heap at 0x380000), then the shared-library band — `libos32gui.shlib` `.text` is shared across PDs, `.data`/`.bss` per app |
| `0x500000–` | External programs: code+bss → sbrk → guard → exec_heap → stack |
| top (256KB+) | Hot-deploy staging, carved out by `sys_usable_mem_end()`; a PEGC/Cirrus 8bpp backbuffer adds ~300KB below it via `sys_reserve_top()`. exec and pgalloc must avoid the whole reservation |

**Subsystem map** — which file does what and which spec section covers it:
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) §2; the task-to-entry-point table is §1.
Three facts that matter on almost every change:

- External programs run at CPL=3 with their own page directory, so a bad pointer kills only the
  app (`fault_kill_count`). The resident shell and `mkos32x --cpl0` binaries are the exceptions.
- SQLite lives in the kernel at 0x200000 with one fixed 384KB MEMSYS5 pool shared by every
  connection (the FEP dictionary included). Close connections at the end of `*_init()`.
- `exec_exit()` reclaims FDs / redirects / pipe buffers **by owner** (exec nest level).
  Kernel-resident FDs need `vfs_fd_set_protect(fd, 1)`; returning to a parent uses
  `exec_heap_restore_state()`, never `exec_heap_init_at()`.

**KernelAPI** — adding or changing one: skill **`os32-kapi-add`**; the procedure's canon is
[`docs/KAPI_SPEC.md`](docs/KAPI_SPEC.md) §3-1 and the rules are [ABI1]–[ABI3]. The struct,
`__cdecl` wrappers, init table and Rust bindings are all generated from `sdk/kapi.json`.

**External programs** — OS32X flat ELF binaries linked with `sdk/link/app.ld` and entered through
`sdk/crt/crt0.asm`; `main()` must be the **first function** in the source file. In-tree sources are
under `userland/`: `shell/` (resident at 0x300000), `cmds/` (25 commands), `system/`, `tests/`,
`rust/` (no_std Cargo workspace), `lib/` (`libos32*`, statically linked). Standard apps and the
board-game RPG are submodules (`apps/`, `game/`) built by `make external` — rebuild them after any
KAPI **or SDK library** change ([`docs/08_build.md`](docs/08_build.md) §8-4).

**Graphics** — CPU writes to VRAM at `0xA8000` ([HW1]): draw into the backbuffer, then
`gfx_present()` flips the page. Modes and the gfx / libos32gfx split:
[`docs/05_drivers.md`](docs/05_drivers.md) §5-5.

## ⚠️ Known Gotchas

短い注意だけ。症状・経緯・検証はリンク先の節にある (§ 番号だけの行は [`docs/POLICY_DEBUG.md`](docs/POLICY_DEBUG.md))。

- Boot-time kselftest: read `kselftest_pass` / `kselftest_fail` at the **new** `kernel.map` address, and
  add a case whenever you touch a kstring / kmalloc / kprintf primitive. → §2
- Fonts: bake with `tools/gen_font16.py` (baseline-anchored, never shrink vertically, judge at 1x). → §4-10
- Kanji in external programs: call `utf8_set_jis_table_ready(1)` yourself, but only after checking a few
  known Unicode→JIS pairs — otherwise every kanji renders as □. → §4-11
- Binaries missing from `deploy.yaml` go stale on the NHD and can hang rshell ([V2]); `make deploy*` prunes
  them via `tools/prune_stale.py` (`NO_PRUNE=1` lists only). → §4-12
- SQLite pool exhaustion shows up as `-2` from `db_query`; always print `db_last_error()` (it returned a kernel pointer and killed CPL=3 apps until 2026-09-17). → §4-13
- `mui_pump_input()` eats the keyboard queue; apps reading keys themselves pass the char via `mui_pump_input_ch()`. → §4-14
- Resource ownership on exit: owner tags, protected FDs, `exec_heap_restore_state`. → §4-15, `docs/10_notes.md` §10-9
- The shell has two heaps, so `kernel/paging.c` must keep 0x380000–0x3FFFFF present. → §4-16
- NHD work image is `build/nhd/os32.nhd` (auto-pulled when missing, NP21/W stopped). Do not trust a
  「配備完了」 line — confirm with kselftest or the `os32-cycle deploy` size check. → §4-17
- The text GDC cursor is controlled only by CSRFORM's DC bit (`console_hw_cursor_enable()` / `_sync()`). → §4-18
- CPL=3 KAPI calls run with IF=1; a `hlt`-waiting wrap hanging with `tick_count` frozen means the
  `sti`/`cli` pair in `int80_stub` broke — the exit must stay IF=0. → §4-19
- GUI internals: `libos32gfx_attach()` (never `gfx_init`), the Cirrus window is mapped once, gshell X4 leaves WM-owned button edges alone. → §4-20〜§4-22
- GUI verification on NP21/W: `--data-urlencode` for `SHIFT+SPACE`, `/api/mouse` uses `ax/ay`, deploy
  rewrites `system.cfg`. → §4-23
- `ext2_g_aux` is the bitmap scratch buffer — never keep an indirect table or data there across a free/alloc
  (files >12KB got cross-linked on overwrite until 2026-09-06). → §4-24
- `mount(dev_id)` gets `(dev_type << 8) | unit` — check the type, or `fd0` opens `hd0`. → §4-30
- "GUI feels slow" → measure first: read `gfx_counters` around the keystroke to see whether a present
  happened at all, then sample `/api/status` `eip` to find where the CPU is. → §4-25
- Never touch the FS from a `sys_ls` callback without a private buffer. → §4-26
- Japanese text is 3 bytes per char and 2 columns wide; `char buf[64]` overflows easily, and truncation
  must land on a UTF-8 boundary. → §4-27
- シリアルの速度は 8253 の**整数分周**で決まる。既定 **9600** は 1.9968MHz / 2.4576MHz の
  どちらでもちょうど出る唯一の標準速度。**38400 は 1.9968MHz で 41600bps に化ける** (+8.3%)。
  クロックは `0000:0501h` bit7 で判定。`0434h` の 4 分周は**極性が未決着なので自動では触らない**。
  **NP21/W は通信速度を模擬していない** — ここは「エミュレータで確認済み」が通用しない。 → §4-49、§4-50
- フロッピーは 2 形式。既定は 2HD 1232KB、`make fd144` が 1.44MB の**生イメージ**を作る。
  **1.44MB の IPL は 512 バイトしか読まれない**、spt=18 はシフトで割れない、ルートDirが SP に当たる、
  FAT は 9 セクタ。D88 は `fd_type=0x21` + 全セクタ `rpm_flg=1` が要り未対応。 → §4-47、§4-48
- FDC の時間上限は**機構の最悪値**から導く (シーク 8ms × 80 トラック、1 回転 200ms)。NP21/W はシーク時間を
  模擬しないので「エミュレータで困らない 200ms」は実機で `root panic` になった (2026-09-22)。
  **1MB 超への DMA は `0439h` bit2 (起動時 1 = 禁止) を落とさないと届かない** — これも NP21/W は見ない。 → §4-51
- `kprintf` の属性は PC-98 流 (bit0 = 表示、bit5-7 = BRG) に**入口で変換**している。「画面に出ている」は
  `/api/tvram` の文字ではなく `/api/screenshot` の**見た目**で確かめる (0x07 の行は 2026-09-22 まで黒かった)。 → §4-52
- FDC の 0x94 は **FRY (bit6) を立てる**。無いと READY 線の無いドライブで全コマンドが Not Ready (NP21/W は通る)。 → §4-53
- Boot loaders: PM transition inlined in `loader_fat.asm`, `boot_fat.asm` is `.8086`, IPL calls INT 1Bh at most 4 times. → [`docs/10_notes.md`](docs/10_notes.md) §10-2, §10-3
- Physical 0x90000 is the auto-play mailbox: change the layout and `game/tools/autoplay/driver.py` in the same commit. → [`docs/02_memory.md`](docs/02_memory.md) §2-1
- 9MB 構成の `v86 -t` は **2026-09-16 に再現しないことを確認** (原因は特定せず解消)。 → §4-28
- 配備の成否は文言で判断しない。**ゲストの `ls -l /boot/vmkernel.lz4` と手元のサイズを
  突き合わせる** ([V4])。コピー失敗自体は 2026-09-10 に非ゼロ終了へ直した。 → §4-29
- `gui_gate.py` で GUI を叩くときは rshell を ESC で抜けてから `/api/key`、Start メニューの行は
  `start_row()` (項目数から導く) を使う — 固定値は 1 行ずれて Shut Down に当たった。 → §4-31
- `ext2_read_file` は端数ブロックを `to_copy` だけ写す (2026-09-11 まで 1KB 溢れていた)。FS の read が
  要求長ちょうどしか書かないと仮定して小さな static バッファへ読まない。 → §4-32
- `hsync` はサイズか日時が違うものだけ内容比較する。同サイズ・同日時で中身が違う差し替えだけ見逃す (`--verify` で全件比較)。置き換えは予約名 `.hs~<名前>` へ書いて検証してから `rename` する — **公開の前**に落ちれば旧内容が残り、**公開の後**に落ちれば新内容が現れて `replace_partial` になる (成功に数えない)。KAPI v53 未満のカーネルでは既定で断る (`--unsafe-overwrite` のときだけ直接上書き)。 → §4-36
- `hsync` は HostDrv の内容で NHD を上書きする。NHD 配備の後は**先に `make deploy`**。 → §4-33
- 保存の試験は**バイト列**で突き合わせる。行の中身だけ見ていたので、末尾の改行を空行と数えて
  **開いて保存するたびに 1 バイト増える**のを見逃していた (穴 H16)。冪等も繰り返して見る。 → §4-46
- アプリが `widget::set_focus()` で移したフォーカスも `on_widget_focus` で返る。捨てていたころは
  **文字は入るのにカーソルキーだけが死んだ** (穴 H13)。通知のある API は入力経由と API 経由の両方を見る。 → §4-44
- Host Services (クリップボード・印刷) は**ホスト側の常駐 `tools/host_agent.py` が要る**
  (`--listen 127.0.0.1:8026`、承認不要)。`-100` = `HOST_ELINK`。初回 open は最大 3 秒待つので
  クリック直後の画面には結果が出ない。**設定は ini ではなく `/api/net` に聞く** — ini を読んで
  「LAN 未設定だから [D2] の承認が要る」と誤判断しかけた。 → §4-45
- GUI アプリの窓が静かに出ない → **共有ライブラリが古い**。`hsync` は既定で `/sys` を外すので
  `libos32gui` を変えたら **`hsync sys` + リセット**。例外 0 件で窓だけ出ないのが目印。 → §4-42
- ビルドは既定で `-j$(nproc)`。`make check` は 2 段 (書き換えない 48 本を並列 → 変異する 10 本を逐次) で **152 秒**。
  各段の後に `tools/check_tree_unchanged.py` がソースの残留を見る。 → §4-41
- `make check` を途中で止めると**変異試験が当てた変更がソースに残る**。打ち切ったら
  コミット前に必ず `git status` を見る (`git add -A` が壊れたコードを拾う)。全 55 本で 15 分以上。 → §4-40
- Device windows: decide from the physical map (`pgalloc_range_has_ram`), never from the RAM ceiling (`sys_get_mem_kb`). → §4-34
- VFS errors are `OS32_ERR_*`, translated at the FS boundary; `vfs_open` refuses directories, `vfs_chdir` refuses non-dirs. → [`docs/06_filesystem.md`](docs/06_filesystem.md) §6-1
- ext2 はメタデータの I/O エラーを 1 回踏むと**そのマウントの間は書き込みを全部断る** (`OS32_ERR_ROFS` = -15)。読み取りは通り、再起動で警告付きで戻る。書き込みが全部 -15 になったらホストの `e2fsck` へ。 → §4-35

## Documentation

ソースツリーの図は [`docs/08_build.md`](docs/08_build.md) §8-3 が正典。

| Document | Content |
|---|---|
| [`docs/INDEX.md`](docs/INDEX.md) | 索引。冒頭の「情報単位ごとの正典」表が**更新先を 1 か所に決める**。§1〜§10 の技術仕様と `docs/tasks/` の領域別設計もここから辿る |
| [`docs/CONSTRAINTS.md`](docs/CONSTRAINTS.md) | 制約規則の正典 ([C1]〜[D3] の理由と詳細) |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | 作業別の参照先 (§1) とファイル地図 (§2) — 未知のサブシステムはここから |
| [`docs/KAPI_SPEC.md`](docs/KAPI_SPEC.md) | KernelAPI 仕様と追加手順 (§3-1) |
| [`docs/POLICY_DEV.md`](docs/POLICY_DEV.md) / [`POLICY_DEBUG.md`](docs/POLICY_DEBUG.md) | 開発規約 / デバッグ (反映確認 §2、教訓集 §4、道具箱 §5) |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | リリース計画 (v1.x GUI シェル〜) |
| `docs/hw/` | PC-9800 ハード資料のミラー (`tools/sync_hwdocs.sh`)。**著作権物・gitignore・コミット禁止**。Bible と矛盾したら UNDOCUMENTED を採る |
| `/home/hight/np21w-src/docs/` | NP21/W ai-debug フォーク。WSL 側が正で、`make build && make deploy` で Windows にミラーされる |

`make docs-win` はこのリポジトリの `docs/` + `README.md` + `CLAUDE.md` を
`C:\WATCOM\docs\os32\` に書き出す (読み取り専用の出力。編集はここ側で行う)。

スキル: **`os32-build-verify`** (ビルド・配備・検証の選択)、**`os32-emu-debug`** (エミュレータ上の障害調査)、
**`os32-kapi-add`** (KernelAPI の追加・変更)、**`os32-emu-config`** (NP21/W ini の限定変更 — [D2] の承認対象)、
**`os32-local-ai`** (ビルド・試験・配備をローカル AI に実行させる)、
**`os32-local-review`** (主レビュアーが枯渇したときの補助レビュー — `tools/review_local.py`)。
