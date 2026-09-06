# CLAUDE.md — AI コーディングアシスタント向けガイダンス

このファイルは AI コーディングアシスタント (Claude Code, Gemini, Hermes 等) が
このリポジトリで作業する際の共通ガイダンスです。**プロジェクト固有の「指示」は
すべてここに集約する** — 規則、日常コマンド、作業別の参照先、短い落とし穴の注意。
分散させると必ず片方が腐る (ビルドターゲット一覧が 4 箇所に散り、`dp-` 廃止のとき
3 箇所が取り残された前例がある)。一方で**技術情報の本文はここに置かない**: 番地・
KAPI 表・ファイル地図・障害の経緯は `docs/INDEX.md`「情報単位ごとの正典」の表が指す
1 か所だけを更新し、ここには要約と参照を置く (2026-09-05)。

Claude Code は本ファイルを自動で読み込む。他のツールから使う場合は、それぞれの
設定でこのファイルを指すこと。

---

## Project Overview

OS32 is a 32-bit bare-metal OS for NEC PC-9801/9821 series machines, built with a GCC i386-elf cross-compiler and NASM. The kernel runs in protected mode at physical address 0x100000 (1MB). External programs are loaded at 0x500000 (0x400000–0x4FFFFF is the shared-library band, GUI v1.1 K3).

## Build Commands

The full target list lives in `docs/08_build.md`; what follows is the
subset you need day to day.

```bash
# Full build (kernel + programs + disk images)
make all

# Kernel only (+ SQLite)
make kernel

# All external programs
make programs

# Individual binary hot-deploy (build + place into the running guest, no reboot).
# Userland only; the kernel and /sys need a full NHD deploy.
make hotdeploy FILE=userland/cmds/wc.bin

# Deploy to HostDrv (C:\os32 on Windows host, no reboot)
make deploy

# Deploy kernel to NHD (requires NP21/W restart)
make deploy-kernel

# Write bootloader to NHD boot sector
make deploy-boot

# Clean all build artifacts
make clean
```

After any kernel change, always run `make kernel` and verify zero errors before deploying.

**Remote testing** (after NP21/W is running):
```bash
curl -X POST http://127.0.0.1:8025/api/cmd --data-binary "ver"  # run a shell command over serial
curl -X POST http://127.0.0.1:8025/api/cmd --data-binary "ls"
curl -X POST http://127.0.0.1:8025/api/key -d "seq=SPACE"       # inject a key event
curl -X POST http://127.0.0.1:8025/api/key -d "seq=SHIFT+SPACE" # FEP on/off toggle
curl      http://127.0.0.1:8025/api/tvram                       # screen contents as UTF-8 text
curl      http://127.0.0.1:8025/api/screenshot                  # capture the emulator window
```
`/api/cmd` can only send whole command lines; use `/api/key` for anything that reads raw
keystrokes (FEP conversion, the editor, games). The debug server is **built into
`np21x64w.exe`** (the ai-debug fork) and is switched on with `aidebug=true` / `aidbport=8025`
in `np21x64w.ini` — no external relay process is involved. Richer inspection (registers,
memory, disassembly, breakpoints, trace) is available through `tools/np21w_mcp/`.

## Compiler & Flags

| Target | Compiler | Key Flags |
|--------|----------|-----------|
| Kernel | i386-elf-gcc | `-std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -O2` |
| SQLite | i386-elf-gcc | `-Os -ffunction-sections -fdata-sections` (size-optimized) |
| External programs | i386-elf-gcc | Same base flags, linked with `sdk/link/app.ld` |
| ASM | NASM | `-f elf32` (kernel), `-f bin` (boot sectors) |

The cross-compiler lives at `$CROSS_DIR/` (default `/usr/local/cross`). The build config is in `build/config.mk`.

## Project Constraints

Violating any of these either breaks the OS or makes verification meaningless.
The lines below are the whole rule; the reasoning and the detail live in
[`docs/CONSTRAINTS.md`](docs/CONSTRAINTS.md), which is the **normative source**.
`make check` verifies this list has not drifted from it.

**C / ABI**

- **[C1]** C89 (GNU89) mandatory — no `//` comments, declarations at block start
  only, no C99 features (`_Bool`, VLAs, `restrict`).
- **[C2]** Inside the kernel use `kstrncpy` / `kstrncat` / `kstrlen` / `kstrcmp`
  from `lib/kstring.h`, never the libc equivalents.
- **[C3]** KernelAPI functions exposed to external programs must have `__cdecl`
  wrappers in `kapi/`. The kernel uses System V i386 ABI internally.
- **[C4]** No hardcoded constants — follow the three-layer constant scheme.

**Hardware**

- **[HW1]** Never use EGC / GRCG / GDC drawing commands. All pixel writes go
  through the CPU directly to VRAM at `0xA8000`.
- **[HW2]** DMA buffers must not straddle a 64KB boundary.

**KernelAPI (ABI)**

- **[ABI1]** `sdk/kapi.json` is the single source of truth. Never hand-edit the
  generated files.
- **[ABI2]** Append entries only. Never reorder or delete an existing slot.
- **[ABI3]** After a KAPI change, bump the version and run `make clean` →
  `make all`. An incremental build breaks silently.

**Verification**

- **[V1]** `make deploy` (HostDrv) alone is not verification — PATH prefers the
  NHD's `/usr/bin`, so an old binary runs silently and looks like a pass.
- **[V2]** Register every launchable binary in its own layer's `deploy.yaml`.
- **[V3]** Do not shorten the curl timeout for remote execution (15s minimum,
  60s+ for long-running programs).
- **[V4]** Report failures and skipped steps as they happened. Never present
  something unverified as a pass.

**Destructive operations**

- **[D1]** Never deploy to the NHD while NP21/W is running. Stop → deploy → start.
- **[D2]** Get approval before irreversible operations (overwriting NHD or disk
  image masters, `rm -rf`, `git reset --hard`, editing `*.ini`).
- **[D3]** Never put the contents of `.env`, API keys or passwords in output.

## Architecture

### Memory Layout

```
0x00000–0x00FFF   NULL guard (not present)
0x01000–0x9FFFF   Conventional memory. Font cache 0x01000, Unicode table
                  0x4A000 (128KB), GFX backbuffer 0x6A000. Also the window
                  handed to the V86 guest (MS-DOS gets the full 640KB).
0x8C000–0x8CFFF   Hot-deploy control block (MEM_HOTDEPLOY_DESC)
0xA0000–0xEFFFF   VRAM (text + graphics planes)
0xF0000–0xFFFFF   BIOS ROM
0x100000–0x1FAFFF Kernel band: binary (.text/.data/.bss) + heap + KAPI + SHM (256KB)
0x1FB000–0x1FBFFF Kernel stack guard (not present)
0x1FC000–0x1FFFFC Kernel stack (16KB)
0x200000–0x2FFFFF SQLite band (1MB): code + BSS + alternate stack (128KB)
0x300000–0x3FFFFF Shell band (1MB): resident binary + guard 0x375000 + stack
0x400000–0x4FFFFF Shared library band (libos32gui.shlib, K3): .text read-only shared by
                  every PD, .data/.bss duplicated per app; original kept at the band's end
0x500000–         External program band: code+bss, then newlib sbrk, guard page,
                  KAPI exec_heap directly below the stack guard (no fixed 1MB cap
                  since 2026-09-04; sbrk/exec_heap split by OS32X heap_size or 50/50)
(top 256KB)       Hot-deploy staging window — carved out of physical memory
                  by sys_usable_mem_end(); exec and pgalloc must avoid it
```

Authoritative definitions live in `include/memmap.h`. The kernel stack moved
from conventional memory to 0x1FC000 so the V86 guest could be handed the
full 640KB — do not reintroduce the old 0x90000 assumption.

### Subsystem map

Which file does what, and which spec section covers it, lives in
`docs/DEVELOPMENT.md` §2 (kernel / exec+KAPI / drivers / fs / gfx+SQLite+boot /
userland). Start there before touching an unfamiliar subsystem; the per-part specs are
`docs/01_system.md` – `docs/10_notes.md`. Facts you need on almost every task:

- External programs run at **CPL=3** with their own page directory; bad pointers kill
  only the app (`fault_kill_count`). The shell (resident at 0x300000) and binaries built
  with `mkos32x --cpl0` are the unprotected exceptions. Model: `docs/09_exec.md`.
- SQLite lives in the kernel at 0x200000 with a fixed 384KB MEMSYS5 pool shared by every
  connection (including the FEP dictionary). Close connections at the end of `*_init()`.
- `exec_exit()` reclaims FDs / redirects / pipe buffers **by owner** (exec nest level);
  kernel-resident FDs must be protected with `vfs_fd_set_protect(fd, 1)`. On return to
  the parent use `exec_heap_restore_state()`, never `exec_heap_init_at()`.

### KernelAPI (`kapi/` + `include/`)

The KernelAPI is the ABI between the kernel and external programs:

- **`sdk/include/os32/os32_kapi_shared.h`** — Single source of truth: `KernelAPI` struct layout, `KAPI_VERSION`, shared types. Both kernel and programs include this.
- **`sdk/include/os32/os32_kapi_generated.h`** — Generated accessor macros.
- **`kapi/kapi_generated.c`** — `__cdecl` wrappers (auto-generated from `sdk/kapi.json`).
- **`kapi/kapi_sys.c`** — System call wrappers.
- **`kapi/kapi_db.c`** — SQLite DB API wrappers.
- **`exec/exec_kapi_init.inc`** — KernelAPI table initialization (included by `exec/exec.c`).

**To add a KernelAPI function** — `sdk/kapi.json` is the single source of truth and the
wrappers/struct/init are all generated from it. Never hand-edit the generated files:

1. Append the entry to the **end** of the `api` array in `sdk/kapi.json` (append-only —
   never reorder or remove existing slots; that breaks the ABI for already-built binaries).
   Add any needed header to `includes` / prototype to `externs` in the same file.
2. Bump `"version"` in `sdk/kapi.json` **and** `KAPI_VERSION` in
   `sdk/include/os32/os32_kapi_shared.h` (they must match).
3. Regenerate and confirm the tree is in sync:
   ```bash
   python3 sdk/gen_kapi.py && python3 sdk/kapi_rust_gen.py
   git diff --stat   # only the intended additions should appear
   ```
   This rewrites `sdk/include/os32/os32_kapi_generated.h`, `kapi/kapi_generated.c`,
   `exec/exec_kapi_init.inc`, and `sdk/rust/os32api/src/kapi_generated.rs`.
4. Implement the target function in the kernel (or give the entry a `target` /
   inline `body` in the JSON).
5. Update `docs/KAPI_SPEC.md`, and bump the required version in `build/app.conf`
   for any program that depends on the new call.

Data fields (plain values rather than function pointers) go in `data_fields`; the generator
emits `kapi-><field> = 0;` and the value must be assigned at runtime in `exec_init()`.

### External Programs (`userland/`)

Programs are OS32X flat ELF binaries linked with `sdk/link/app.ld`, starting with `sdk/crt/crt0.asm`. The `main()` function must be the **first function** in the source file; helpers go after `main()` with forward declarations.

#### Directory Structure

`userland/` builds inside the OS tree. 標準アプリとゲームは別リポジトリに
分離済み — SDK だけでビルドし、このリポジトリの成果物には含まれない:

- **`ske-studio/os32-apps`** — edit / mdview / mgxview / ui_demo / vbzview /
  vdpview / ekakiuta / raster / gfx_demo / demo1 / spr_test / hello32
- **`ske-studio/os32-game`** — 対戦スゴロク RPG (`app/`, `lib/`, `assets/`, `data/`)

どちらも **git submodule** として `apps/` と `game/` に置き (2026-09-04)、`make sdk` で
生成した `build/sdk/` を指してビルドする:

```bash
git submodule update --init          # 初回 / clone 直後
make external                        # apps + game (make apps / make game で個別)
```

検証した組み合わせは submodule のポインタとして os32 のコミットに残る。KAPI を
動かしたら `make external` で両方を再ビルドし、ポインタを更新してコミットする。
**SDK のライブラリ (libos32gfx 等) を変えたときも同様** — アプリは静的リンクなので、
古い .bin は新しいバックエンド (PEGC の PACKED8 等) で #PF する (2026-09-06 hello32 で実測)。
`apps/deploy.yaml` と `game/deploy.yaml` は配備マニフェストに統合される
(`tools/deploy_manifests.py`)。emu_agent の `make` は `apps` / `game` / `external` を
許可リストに含む。

ゲームエンジンのライブラリ (chem / board / ai / battle / econ / event / inv /
map / rpg / text / turn) は os32-game リポジトリが自前のソースからビルドする。
このリポジトリの `build/libs.mk` は userland/lib の 14 ライブラリだけを扱う。

| Directory | Content |
|-----------|---------|
| `userland/shell/` | System shell (resident at 0x300000, modular: 12+ source files) |
| `userland/cmds/` | CLI commands: `grep`, `less`, `sort`, `diff`, `find`, `wc`, `hexdump`, `man`, etc. (18 commands) |
| `userland/system/` | System utilities: `hsync` (HostDrv sync), `install`, `cdinst`, `sndctl`, `lz4` |
| `userland/tests/` | Test/demo programs (36 sources including per-library test suites) |
| `userland/rust/` | Rust programs (Cargo workspace: `alloc_demo`, `hello_gfx`, etc.) |
| `userland/lib/` | User-space libraries (map: `docs/DEVELOPMENT.md` §2) |

Shell modules and the per-library descriptions are in `docs/DEVELOPMENT.md` §2
(userland). Rule for libraries: statically linked, `libos32*` prefix, Rust crates are
`no_std` with no external crates (v2 CONTRACTS C8).

### Graphics

All pixel writes go through CPU directly to VRAM at `0xA8000` — see **[HW1]**. Flow: draw to
backbuffer → `gfx_present()` → VRAM with automatic page flip (no VSYNC wait). Modes, page flip,
and the gfx/libos32gfx split: `docs/05_drivers.md` §5-5. The 9821 backends (PEGC, Cirrus) are
planned via the HAL in `docs/tasks/gui/DESIGN.md`.

### Deploy Workflow

Three deployment paths:
1. **HostDrv** (`make deploy`): Copies build artifacts to `C:\os32` (Windows). Guest OS reads files via `/host` mount. No reboot required. Fast iteration path.
2. **NHD** (`make deploy-kernel`): Syncs HostDrv, then writes the whole tree (kernel + programs + data) into the NHD ext2 image. **Requires NP21/W to be stopped first** and restarted afterwards.
3. **Boot sector** (`make deploy-boot`): Writes `boot/loader_hdd.bin` to the NHD boot area (LBA 2–17). Only needed when the loader itself changed.

Deployment manifests are split by owning layer (`build/core.yaml`,
`userland/deploy.yaml`); the list
and the merge live in `tools/deploy_manifests.py`. Environment variables: `HOSTDRV_DIR` (default `/mnt/c/os32`), `NP21W_DIR` (default `/tmp/np21w`).

Build artifacts land in `build/out/` (gitignored), not the repository root.

## ⚠️ Known Gotchas

Short form only. The history, symptoms and verification for each item are in
`docs/POLICY_DEBUG.md` §4 (section numbers below); update the story there, not here.

- **Boot-time kernel selftest** (`kernel/kselftest.c`) runs every boot and prints failing
  items in red; read `kselftest_pass` / `kselftest_fail` with `emu_read_mem`. Add a case
  whenever you touch a kstring / kmalloc / kprintf primitive — `userland/tests/klibc_test`
  links newlib and does not cover the kernel side.
- **Fonts**: bake with `tools/gen_font16.py` (baseline-anchored, never shrink vertically,
  judge at 1x). Gothic for 16x16 kanji. → §4-10
- **Kanji in external programs**: call `utf8_set_jis_table_ready(1)` yourself, but only after
  verifying a few known Unicode→JIS pairs; otherwise every kanji renders as □. → §4-11
- **Japanese text width**: 3 bytes per char in UTF-8, 2 columns (16px) on screen; `char buf[64]`
  overflows easily. Truncate only on UTF-8 boundaries.
- **Physical 0x90000** is the auto-play mailbox (game writes a state block each frame, host reads
  `GET /api/mem?addr=0x90000&space=phys`). Changing the layout means updating
  `tools/autoplay/driver.py` `read_mailbox()` and EXPORT_VERSION. Layout: `docs/02_memory.md`.
- **Binaries missing from deploy.yaml go stale on the NHD** and can hang rshell after a KAPI
  change ([V2]). `make deploy*` now prunes them via `tools/prune_stale.py`
  (`NO_PRUNE=1` to list only). → §4-12
- **HostDrv deploy never overrides `/usr/bin`** — the NHD binary runs instead ([V1]).
  Verify library/test changes with a full NHD deploy.
- **SQLite pool exhaustion** shows up as `-2` from `db_query`; always print
  `db_last_error()` when diagnosing, and use `dbq` to inspect a DB on the target. → §4-13
- **`mui_pump_input()` eats the keyboard queue**; apps that read keys themselves must pass
  the char via `mui_pump_input_ch(ctx, ch)`. → §4-14
- **Resource ownership on exit** (owner tags, protected FDs, `exec_heap_restore_state`): see the
  Subsystem map above. → §4-15, spec `docs/10_notes.md` §10-9
- **The shell has two heaps** (newlib sbrk below `MEM_SHELL_GUARD`, KAPI exec_heap at
  `MEM_SHELL_HEAP_BASE` 0x380000); `kernel/paging.c` must keep 0x380000–0x3FFFFF present. → §4-16
- **Never touch the FS from a `sys_ls` callback without a private buffer** — `ext2_list_dir`
  copies each block first because a writing callback clobbers `ext2_g_aux`. FatFs / HostDrv
  list_dir are not audited.
- **VFS error codes are `OS32_ERR_*`** (`os32_kapi_shared.h`); FS drivers translate at the
  boundary (`ext2_to_vfs_err`). `vfs_open` refuses directories, `vfs_chdir` refuses non-dirs.
- **NHD work image is `build/nhd/os32.nhd`** (auto-pulled from Windows when missing, NP21/W
  stopped). Do not trust "配備完了" — confirm with kselftest at the new kernel.map address or the
  `os32-cycle deploy` size check. → §4-17
- **Text GDC cursor** is controlled only by CSRFORM's DC bit; use `console_hw_cursor_enable()` /
  `console_hw_cursor_sync()`. → §4-18
- **CPL=3 KAPI calls run with IF=1** (`int80_stub` does `sti` after the segment reload and
  `cli` before `iretd`). A `hlt`-waiting wrap hanging with `tick_count` frozen means that
  pair was broken; the exit must stay IF=0 or an IRQ leaves DS=KERNEL_DS for CPL=3. → §4-19
- **Boot loaders**: PM transition stays inlined in `boot/loader_fat.asm`; `boot_fat.asm` is
  `.8086` (no immediate shifts); the IPL may call INT 1Bh at most 4 times on NP21/W.
  → `docs/10_notes.md` §10-2, §10-3

## Source Tree

The directory tree is maintained in one place: `docs/08_build.md` §8-3. Top level:
`boot/` `kernel/` `exec/` `fs/` `drivers/` `gfx/` `kapi/` `lib/` `include/` (kernel side),
`userland/` (built against the SDK), `apps/` `game/` (submodules), `sdk/`, `tools/`, `build/`
(artifacts in `build/out/`, work NHD in `build/nhd/`), `assets/`, `docs/` (`docs/hw/` is the
gitignored hardware mirror).

## Documentation

| Document | Content |
|----------|---------|
| `docs/INDEX.md` | Document index. Its first table, **情報単位ごとの正典**, says which file to update for each kind of fact |
| `docs/01_system.md` ~ `docs/10_notes.md` | Kernel technical spec (10 parts) |
| `docs/KAPI_SPEC.md` | KernelAPI spec (current: **v42**, 180 functions + 2 data fields) |
| `docs/DEVELOPMENT.md` | Development guide: task → what to read / touch / verify, and the file map (file → role → spec section) |
| `docs/POLICY_DEV.md` | Coding policy, build rules |
| `docs/POLICY_DEBUG.md` | Debug procedures, binary deployment checklist, **§4 lessons (the long form of Known Gotchas)** |
| `docs/ROADMAP.md` | Release roadmap (v1.1 GUI shell~) |
| `docs/tasks/fep/` | FEP (Japanese input) detailed design, P1–P7 |
| `docs/tasks/gui/` | GUI shell v1.x: `DESIGN.md` (design record), `API_CONTRACTS.md` (frozen 2026-09-04), `TASKS.md` + `TASK_*.md` (lane tickets H/K/W/C, gates G1–G5) |
| `docs/tasks/game/` | Board-game RPG port plan + engine extension plan |
| `docs/tasks/wintree_port/` | Record of the feat/vdm work-tree port |
| `/home/hight/np21w-src/docs/` | **NP21/W ai-debug fork** (AI-native emulator debugging: embedded HTTP debug server + MCP). Plan, build setup, architecture. WSL repo is the source of truth; build and deploy from WSL with `make build && make deploy` (mirrors to Windows and drives MSBuild) |

For PC-9800 hardware specs, read the local mirror `docs/hw/` (`PC9800Bible/` = PC-9801
Bible, `undocumented/` = UNDOCUMENTED 9801/9821 Vol.2 `io_*.md`). The source of truth is
`C:\WATCOM\docs\` (`/mnt/c/WATCOM/docs/`); refresh the mirror with `tools/sync_hwdocs.sh`.
**The mirror is copyrighted material and is gitignored (`/docs/hw/`) — never commit it.**
The reverse direction exists too: `make docs-win` (`tools/sync_docs_to_win.sh`) mirrors this
repository's `docs/` + `README.md` + `CLAUDE.md` to `C:\WATCOM\docs\os32\` for reading from
Windows. That copy is read-only output — edit the files here, never there.
When the two books disagree, UNDOCUMENTED wins (see `docs/INDEX.md`).
