# GEMINI.md — AI コーディングアシスタント向けガイダンス

このファイルは AI コーディングアシスタント (Gemini, Claude Code 等) がこのリポジトリで
作業する際の共通ガイダンスです。

> **他の AI 向け設定ファイル:** `CLAUDE.md` はこのファイルを参照しています。

---

## Project Overview

OS32 is a 32-bit bare-metal OS for NEC PC-9801/9821 series machines, built with a GCC i386-elf cross-compiler and NASM. The kernel runs in protected mode at physical address 0x100000 (1MB). External programs are loaded at 0x400000.

## Build Commands

```bash
# Full build (kernel + programs + disk images)
make all

# Kernel only (+ SQLite)
make kernel

# All external programs
make programs

# Individual program hot-deploy (build + push via serial without reboot)
make dp-<name>        # e.g. make dp-shell

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
curl -X POST http://localhost:8032/cmd -d "ver"     # run a shell command over serial
curl -X POST http://localhost:8032/cmd -d "ls"
curl -X POST http://localhost:8032/key -d "SPACE"   # inject a key event into the NP21/W window
curl -X POST http://localhost:8032/key -d "SHIFT_SPACE"   # FEP on/off toggle
curl      http://localhost:8032/screenshot           # capture the emulator window
```
`/cmd` can only send whole command lines; use `/key` for anything that reads raw keystrokes
(FEP conversion, the editor, games). The server is `tools/os32_server.py`, run on the Windows
side — **it is copied to `C:\os32tools\` manually, so re-copy it after editing.**

## Compiler & Flags

| Target | Compiler | Key Flags |
|--------|----------|-----------|
| Kernel | i386-elf-gcc | `-std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -O2` |
| SQLite | i386-elf-gcc | `-Os -ffunction-sections -fdata-sections` (size-optimized) |
| External programs | i386-elf-gcc | Same base flags, linked with `build/app.ld` |
| ASM | NASM | `-f elf32` (kernel), `-f bin` (boot sectors) |

The cross-compiler lives at `$CROSS_DIR/` (default `/usr/local/cross`). The build config is in `build/config.mk`.

## Coding Rules

**C89 (GNU89) mandatory** — the entire codebase uses `-std=gnu89`:
- No `//` comments — use `/* */`
- Variables declared at block start only (no `for (int i = ...)`)
- No C99 features: no `_Bool`, no VLAs, no `restrict`

**Calling convention** — KernelAPI functions exposed to external programs must have `__cdecl` wrappers in `kapi/`. The kernel uses System V i386 ABI internally.

**String functions** — use `kstrncpy`, `kstrncat`, `kstrlen`, `kstrcmp` from `lib/kstring.h` instead of libc equivalents inside the kernel.

## Architecture

### Memory Layout

```
0x00000–0x0FFFF   Conventional memory (font cache, Unicode table, GFX backbuffer)
0x90000–0x9FFFF   Kernel stack (64KB, guard page at 0x8F000)
0xA0000–0xEFFFF   VRAM (text + graphics planes)
0x100000          Kernel binary (.text/.data/.bss, ~200KB), then heap
0x200000          SQLite code + BSS (~579KB) + alternate stack
0x300000          Shell resident binary (~113KB) + stack
0x400000          External program load area (max 1MB) + stack
```

### Kernel Subsystems (`kernel/`)

- **paging / pgalloc**: Page table management, guard pages, physical page allocator.
- **kmalloc**: Kernel heap allocator.
- **exec / exec_heap**: External program loader (OS32X flat binary format). Manages program heap via `exec_heap.c`.
- **shm**: Shared memory regions (used for SQLite result passing).
- **ime**: Japanese input FEP (romaji→kana→kanji). `ime_romkana.c` (romaji conversion),
  `ime_dict.c` (SQLite dictionary + user dictionary), `ime_render.h` / `ime_render_tvram.c`
  (drawing backend behind a function-pointer table, so the candidate window can move to
  GFX mode later). Candidate list navigation: ↑↓ / number keys / page up-down.
  Design docs: `docs/tasks/fep/`.
- **console**: TVRAM text output. `tvram_set_scroll_reserve(rows)` keeps the bottom N rows
  out of the scroll region so the FEP preedit/candidate window is not carried away by
  scrolling body text.
- **snd_engine**: FM/SSG sound sequencer running on timer IRQ.

### Drivers (`drivers/`)

- `fdc.c` / `disk.c` — floppy controller + logical disk abstraction
- `ide.c` / `atapi.c` — IDE/ATAPI (HDD, CD-ROM)
- `loop_dev.c` — loopback block device (mounts disk images as block devices)
- `dev.c` — unified device registry (`dev_register`, `dev_blk_read/write`)
- `kbd.c` — PC-98 keyboard + V86 scancode injection
- `serial.c` — RS-232C (used for remote test API)
- `mouse.c` / `mouse_bus.c` / `mouse_seamless.c` — bus mouse + NP21/W seamless mouse
- `fm.c` — OPN/OPM FM synthesis chip
- `kcg.c` — KCG (PC-98 character generator) font rendering
- `np2sysp.c` — NP21/W system port hypercalls

### Filesystem (`fs/`)

- `vfs.c` / `vfs_fd.c` — VFS layer, file descriptor table
- `ext2_*.c` — ext2 read/write (primary FS on HDD/NHD)
- `fatfs/` — FatFs port for FAT12/16 floppy (the only FAT implementation; the
  hand-written `fs/fat12.c` was removed — see `docs/06_filesystem.md`)
- `fatfs_vfs.c` — VFS adapter for FatFs
- `hostdrvfs.c` — HostDrv: access Windows host filesystem via NP21/W hypercalls
- `pipe_buffer.c` / `fd_redirect.c` — pipe and I/O redirection infrastructure
- `iso9660.c` — CD-ROM read-only FS

### SQLite Integration (`lib/sqlite3/`)

SQLite is embedded in the kernel at 0x200000 as a separate code section:

- `sqlite3.c` / `.h` — SQLite amalgamation build
- `os32_sqlite_config.h` — OS32 config (MEMSYS5 allocator, no FPU)
- `os32_sqlite_vfs.c` — Custom VFS mapping file I/O to kernel functions
- `sqlite_stack.asm` — Alternate stack setup (SQLite needs deep stack)

KernelAPI DB functions (`kapi/kapi_db.c`): `db_open`, `db_close`, `db_exec`, `db_prepare`, `db_step`, `db_column_int/text`, `db_finalize`, `db_last_error`, `db_mem_used`. DB results are passed via shared memory (`MEM_SHM_BASE`). Max concurrent connections: `DB_MAX_CONNECTIONS`.

### Boot (`boot/`)

- `boot_fat.asm` — FAT boot sector (floppy, .8086 mode)
- `boot_hdd.asm` — HDD boot sector
- `loader_fat.asm` — FAT loader (includes PM transition — do not split)
- `loader_hdd.asm` — HDD loader (ext2)
- `boot_main.c` — Boot main (C, runs after PM transition)
- `ext2_mini.c` — Minimal ext2 reader for boot
- `lz4_mini.c` — Minimal LZ4 decoder for kernel decompression

### KernelAPI (`kapi/` + `include/`)

The KernelAPI is the ABI between the kernel and external programs:

- **`include/os32_kapi_shared.h`** — Single source of truth: `KernelAPI` struct layout, `KAPI_VERSION`, shared types. Both kernel and programs include this.
- **`include/os32_kapi_generated.h`** — Generated accessor macros.
- **`kapi/kapi_generated.c`** — `__cdecl` wrappers (auto-generated from `tools/kapi.json`).
- **`kapi/kapi_sys.c`** — System call wrappers.
- **`kapi/kapi_db.c`** — SQLite DB API wrappers.
- **`exec/exec_kapi_init.inc`** — KernelAPI table initialization (included by `exec/exec.c`).

**To add a KernelAPI function** — `tools/kapi.json` is the single source of truth and the
wrappers/struct/init are all generated from it. Never hand-edit the generated files:

1. Append the entry to the **end** of the `api` array in `tools/kapi.json` (append-only —
   never reorder or remove existing slots; that breaks the ABI for already-built binaries).
   Add any needed header to `includes` / prototype to `externs` in the same file.
2. Bump `"version"` in `tools/kapi.json` **and** `KAPI_VERSION` in
   `include/os32_kapi_shared.h` (they must match).
3. Regenerate and confirm the tree is in sync:
   ```bash
   python3 tools/gen_kapi.py && python3 tools/kapi_rust_gen.py
   git diff --stat   # only the intended additions should appear
   ```
   This rewrites `include/os32_kapi_generated.h`, `kapi/kapi_generated.c`,
   `exec/exec_kapi_init.inc`, and `programs/rust/os32api/src/kapi_generated.rs`.
4. Implement the target function in the kernel (or give the entry a `target` /
   inline `body` in the JSON).
5. Update `docs/KAPI_SPEC.md`, and bump the required version in `build/app.conf`
   for any program that depends on the new call.

Data fields (plain values rather than function pointers) go in `data_fields`; the generator
emits `kapi-><field> = 0;` and the value must be assigned at runtime in `exec_init()`.

### External Programs (`programs/`)

Programs are OS32X flat ELF binaries linked with `build/app.ld`, starting with `programs/crt0.asm`. The `main()` function must be the **first function** in the source file; helpers go after `main()` with forward declarations.

#### Directory Structure

| Directory | Content |
|-----------|---------|
| `shell/` | System shell (resident at 0x300000, modular: 12+ source files) |
| `apps/` | GUI applications: `edit/` (VZ-style editor), `game/` (board-game RPG, WIP), `ui_demo/` (microUI), `mdview`, `ekakiuta`, `vdpview`, etc. |
| `cmds/` | CLI commands: `grep`, `less`, `sort`, `diff`, `find`, `wc`, `hexdump`, `man`, etc. (17 commands) |
| `system/` | System utilities: `hsync` (HostDrv sync), `install`, `cdinst`, `sndctl`, `lz4` |
| `tests/` | Test/demo programs (45 programs including per-library test suites) |
| `rust/` | Rust programs (Cargo workspace: `alloc_demo`, `hello_gfx`, etc.) |

#### Shell Architecture (`programs/shell/`)

Modular design with command registration mechanism (`ShellCmd` struct, max 128 commands):
- `main.c` — Entry, command router, pipe execution, wildcard expansion
- `ui.c` — Main loop, line editing, tab completion, command history
- `cmd_base.c` / `cmd_file.c` / `cmd_dir.c` / `cmd_mnt.c` / `cmd_sys.c` / `cmd_env.c` — Command modules
- `cmd_script.c` — Script engine (if/else/for/while/source, max 128 lines, 4-deep nesting)
- `rshell.c` — Remote shell via serial

#### Libraries (`programs/lib*/`)

All statically linked. Organized by layer:

**Infrastructure:**

| Library | Description |
|---------|-------------|
| `libos32` | Common utilities: debug serial output, man page display, PKG package extraction |
| `libos32math` | Integer math (fixed-point, sin/cos LUT, vec2, sqrt, atan2, random) |
| `libos32input` | Input abstraction (keyboard/mouse → action bindings) |
| `libos32gfx` | Graphics (bezier, sprites, BMP save, raster effects, scale2x) |
| `libos32snd` | FM/SSG sound |
| `libos32db` | SQLite user-space wrapper (`db_open/close/exec/query/step/column_*`) |

**UI / Rendering:**

| Library | Description |
|---------|-------------|
| `libos32tilemap` | SFC-style 4-plane BG tile map compositing engine (C + NASM optimized) |
| `libos32ui` | microUI port (immediate-mode GUI) with GFX renderer bridge |
| `libos32filer` | GFX file browser (modal file selection UI) |
| `libos32md` | Markdown parser + GFX renderer |

**Game Engine:**

> These libraries read their master data into a RAM cache during `*_init()` and then
> **close the DB connection immediately**. Keeping connections open exhausts the fixed
> SQLite MEMSYS5 pool once several engines are initialized in one program.

| Library | Description |
|---------|-------------|
| `libos32ai` | Score-based AI decision engine |
| `libos32asset` | Asset/resource lifecycle management |
| `libos32battle` | Turn-based battle resolution engine |
| `libos32board` | Node-graph board game engine |
| `libos32chem` | BotW-style chemistry engine (SQLite-backed) |
| `libos32econ` | Turn-based data-driven economy simulation (+ estate subsystem) |
| `libos32ecs` | Entity-Component-System game object management |
| `libos32event` | Event scheduler (turn/weekly/conditional/probability triggers) |
| `libos32inv` | Inventory, equipment, shop engine |
| `libos32map` | RPG map management (SQLite-backed, tiled, 3 layers) |
| `libos32rpg` | Persistent character growth: EXP curve, level-up, field status ticks, death/reborn, ranking (SQLite-backed) |
| `libos32save` | Save-state management: region registration, magic/version/CRC32 verification, migration callback |
| `libos32text` | RPG/ADV text management engine |
| `libos32turn` | Multi-player turn rotation, week boundaries, max turns, skip handling |

### Graphics

All pixel writes go through CPU directly to VRAM at `0xA8000` (graphics planes). Never use EGC/GRCG/GDC hardware accelerators — hardware bugs and emulator compatibility issues. Flow: draw to backbuffer → `gfx_present()` → VRAM with page flip (port A4h/A6h). Page flip is automatic; no VSYNC wait needed.

### Deploy Workflow

Three deployment paths:
1. **HostDrv** (`make deploy`): Copies build artifacts to `C:\os32` (Windows). Guest OS reads files via `/host` mount. No reboot required. Fast iteration path.
2. **NHD** (`make deploy-kernel`): Syncs HostDrv, then writes the whole tree (kernel + programs + data) into the NHD ext2 image. **Requires NP21/W to be stopped first** and restarted afterwards.
3. **Boot sector** (`make deploy-boot`): Writes `boot/loader_hdd.bin` to the NHD boot area (LBA 2–17). Only needed when the loader itself changed.

Configuration in `tools/deploy.yaml`. Environment variables: `HOSTDRV_DIR` (default `/mnt/c/os32`), `NP21W_DIR` (default `/tmp/np21w`).

Build artifacts land in `build/out/` (gitignored), not the repository root.

## ⚠️ Known Gotchas

**ABI break after KernelAPI change**: After modifying `KernelAPI` struct, run `make clean` before rebuilding. Stale `.o` files with old struct layout cause silent corruption.

**KAPI にポインタ検証はない (設計上の制約)**: GDT にユーザディスクリプタが
なく、外部プログラムも CPL=0 で実行される。KAPI に渡されたポインタ/サイズは
検証されずカーネルがそのまま使うため、プログラムのバグは即カーネル破壊に
なり得る。リング 3 導入はアーキテクチャ変更 (GDT/TSS/ゲート全面改修) で
2026-08 の信頼性向上 (Phase 1-3) ではスコープ外と判断した。
呼び出し側 (プログラム) が防衛的に書くこと。

**deploy.yaml に無いバイナリは NHD 上で stale 化する**: KAPI レイアウトが
変わると旧バイナリの KAPI 呼び出しが別関数へ飛び、exit 後の `jmp $` で
永久スピンして rshell ごと沈黙する (2026-08-13 に sndctl で実測)。
起動対象になるバイナリは PKG 配布でも必ず deploy.yaml に載せる。

**HostDrv deploy does not override `/usr/bin`**: `make deploy` only writes to `C:\os32`
(the guest's `/host`). PATH resolution prefers the NHD's `/usr/bin`, so running a program
by name after a HostDrv-only deploy silently executes the **old** binary on the NHD.
Library or test changes must be verified with a full NHD deploy
(stop NP21/W → `make deploy-kernel` → restart). Serial hot deploy
(`nhd_deploy.py push`) is not an alternative while `os32_server.py` holds the named pipe.

**SQLite MEMSYS5 is a fixed 200KB pool** (`lib/sqlite3/os32_sqlite_vfs.c`): shared by every
DB connection including the kernel-side FEP dictionary. Programs that hold several
connections open at once exhaust it and get `-2` from `db_query`. Engine libraries avoid
this by closing the connection at the end of `*_init()`.

**`exec_exit()` closes every FD ≥ 3 on program exit** (`exec/exec.c`): any kernel-resident
file descriptor that must outlive user programs (e.g. the FEP dictionary's SQLite
connection) must be protected with `vfs_fd_set_protect(fd, 1)`, or it gets silently
reclaimed and subsequent I/O on it fails (this was the root cause of the 2026-08-07
"FEP zero candidates" bug — every `sqlite3_step` returned `SQLITE_IOERR_READ`).

**FAT12 loader PM transition**: Protected-mode transition code is inlined in `boot/loader_fat.asm`. Do not split it to a separate file — address overlap causes data corruption.

**WASM dot-label bug** (legacy ASM files): In `.386p` + `USE16` mode, dot-prefixed local labels (`.wait:`) cause wasm to silently skip object generation. Use plain labels (`wait_bsy:`) instead.

**boot_fat.asm is .8086 mode**: No immediate shifts — use `mov cl, N / shr ax, cl` instead of `shr ax, N`.

**NP21/W INT 1Bh limit**: IPL may call INT 1Bh at most 4 times. `boot_fat.asm` is already minimal; do not add calls.

## Source Tree

```
os32/
├── boot/             — Boot loaders (NASM + C, FAT/ext2/HDD)
├── kernel/           — Kernel core (main, paging, IDT, V86, IME, sound engine)
├── exec/             — Program loader / KernelAPI initialization
├── fs/               — Filesystems (VFS, ext2, FatFs, iso9660, HostDrv, pipes)
├── drivers/          — Device drivers (IDE, ATAPI, FDC, KBD, Mouse, Serial, FM, KCG)
├── gfx/              — Graphics (CPU backbuffer layer, page flip)
├── kapi/             — KernelAPI __cdecl wrappers (including auto-generated)
├── lib/              — Kernel libraries (kstring, kprintf, UTF-8/16, path, sqlite3)
├── include/          — Shared headers (os32_kapi_shared.h, etc.)
├── programs/
│   ├── shell/        — System shell (modular, resident at 0x300000)
│   ├── apps/         — Applications (edit/, game/, ui_demo/, mdview, ekakiuta, vdpview, etc.)
│   ├── cmds/         — CLI commands (grep, less, sort, diff, find, wc, etc.)
│   ├── system/       — System utilities (hsync, install, cdinst, sndctl)
│   ├── tests/        — Test/demo programs (40+)
│   ├── rust/         — Rust programs (Cargo workspace)
│   └── libos32*/     — User-space libraries (24 libraries, all libos32-prefixed)
├── tools/            — Host-side tools (Python scripts, kapi.json, deploy.yaml)
├── docs/             — Documentation (specs, policies, tasks, manpages)
├── build/            — Build config (Makefiles, linker scripts)
│   └── out/          — Build artifacts (kernel.bin, sqlite.bin, vmkernel.lz4,
│                       unicode.bin, kernel.elf, kernel.map) — gitignored
├── assets/           — Game assets
├── GEMINI.md         — This file (AI guidance)
└── Makefile          — Top-level build script
```

## Documentation

| Document | Content |
|----------|---------|
| `docs/INDEX.md` | Document index (master reference) |
| `docs/01_system.md` ~ `docs/10_notes.md` | Kernel technical spec (10 parts) |
| `docs/KAPI_SPEC.md` | KernelAPI spec (current: **v35**, 164 functions + 2 data fields) |
| `docs/DEVELOPMENT.md` | Architecture, subsystem details |
| `docs/POLICY_DEV.md` | Coding policy, build rules |
| `docs/POLICY_DEBUG.md` | Debug procedures, binary deployment checklist |
| `docs/ROADMAP.md` | Release roadmap (v1.1 GUI shell~) |
| `docs/tasks/fep/` | FEP (Japanese input) detailed design, P1–P7 |
| `docs/tasks/game/` | Board-game RPG port plan + engine extension plan |
| `docs/tasks/wintree_port/` | Record of the feat/vdm work-tree port |
| `/home/hight/np21w-src/docs/` | **NP21/W ai-debug fork** (AI-native emulator debugging: embedded HTTP debug server + MCP). Plan, build setup, architecture. WSL repo is the source of truth; build and deploy from WSL with `make build && make deploy` (mirrors to Windows and drives MSBuild) |

For PC-9800 hardware specs, refer to `docs/PC9800Bible/`.
