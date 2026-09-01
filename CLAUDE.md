# CLAUDE.md — AI コーディングアシスタント向けガイダンス

このファイルは AI コーディングアシスタント (Claude Code, Gemini, Hermes 等) が
このリポジトリで作業する際の共通ガイダンスです。**プロジェクト固有の指示は
すべてここに集約する。** 分散させると必ず片方が腐る (ビルドターゲット一覧が
4 箇所に散り、`dp-` 廃止のとき 3 箇所が取り残された前例がある)。

Claude Code は本ファイルを自動で読み込む。他のツールから使う場合は、それぞれの
設定でこのファイルを指すこと。

---

## Project Overview

OS32 is a 32-bit bare-metal OS for NEC PC-9801/9821 series machines, built with a GCC i386-elf cross-compiler and NASM. The kernel runs in protected mode at physical address 0x100000 (1MB). External programs are loaded at 0x400000.

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
make hotdeploy FILE=apps/edit/edit.bin

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
- **[ABI4]** KAPI does not validate pointers. The caller must be defensive —
  external programs run at CPL=0, so their bugs corrupt the kernel.

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
0x400000–         External program load area (max 1MB) + heap + stack
(top 256KB)       Hot-deploy staging window — carved out of physical memory
                  by sys_usable_mem_end(); exec and pgalloc must avoid it
```

Authoritative definitions live in `include/memmap.h`. The kernel stack moved
from conventional memory to 0x1FC000 so the V86 guest could be handed the
full 640KB — do not reintroduce the old 0x90000 assumption.

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

### External Programs (`userland/`, `apps/`, `game/`)

Programs are OS32X flat ELF binaries linked with `sdk/link/app.ld`, starting with `sdk/crt/crt0.asm`. The `main()` function must be the **first function** in the source file; helpers go after `main()` with forward declarations.

#### Directory Structure

The tree is split by ownership. `userland/` builds inside the OS tree; `apps/`
and `game/` build against the staged SDK alone and could be lifted into their
own repositories unchanged.

| Directory | Content |
|-----------|---------|
| `userland/shell/` | System shell (resident at 0x300000, modular: 12+ source files) |
| `userland/cmds/` | CLI commands: `grep`, `less`, `sort`, `diff`, `find`, `wc`, `hexdump`, `man`, etc. (17 commands) |
| `userland/system/` | System utilities: `hsync` (HostDrv sync), `install`, `cdinst`, `sndctl`, `lz4` |
| `userland/tests/` | Test/demo programs (45 programs including per-library test suites) |
| `userland/rust/` | Rust programs (Cargo workspace: `alloc_demo`, `hello_gfx`, etc.) |
| `userland/lib/` | User-space libraries (see below) |
| `apps/` | Standard applications: `edit/` (VZ-style editor), `ui_demo/` (microUI), `mdview`, `mgxview`, `vbzview`, `vdpview`, `ekakiuta`, `raster`, `gfx_demo`, `demo1`, `spr_test`, `hello32` |
| `game/` | The board-game RPG: `app/`, `lib/`, `assets/`, `data/` |

#### Shell Architecture (`userland/shell/`)

Modular design with command registration mechanism (`ShellCmd` struct, max 128 commands):
- `main.c` — Entry, command router, pipe execution, wildcard expansion
- `ui.c` — Main loop, line editing, tab completion, command history
- `cmd_base.c` / `cmd_file.c` / `cmd_dir.c` / `cmd_mnt.c` / `cmd_sys.c` / `cmd_env.c` — Command modules
- `cmd_script.c` — Script engine (if/else/for/while/source, max 128 lines, 4-deep nesting)
- `rshell.c` — Remote shell via serial

#### Libraries (`userland/lib/`, `game/lib/`)

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

All pixel writes go through CPU directly to VRAM at `0xA8000` (graphics planes) — see **[HW1]**. Flow: draw to backbuffer → `gfx_present()` → VRAM with page flip (port A4h/A6h). Page flip is automatic; no VSYNC wait needed.

### Deploy Workflow

Three deployment paths:
1. **HostDrv** (`make deploy`): Copies build artifacts to `C:\os32` (Windows). Guest OS reads files via `/host` mount. No reboot required. Fast iteration path.
2. **NHD** (`make deploy-kernel`): Syncs HostDrv, then writes the whole tree (kernel + programs + data) into the NHD ext2 image. **Requires NP21/W to be stopped first** and restarted afterwards.
3. **Boot sector** (`make deploy-boot`): Writes `boot/loader_hdd.bin` to the NHD boot area (LBA 2–17). Only needed when the loader itself changed.

Deployment manifests are split by owning layer (`build/core.yaml`,
`userland/deploy.yaml`, `apps/deploy.yaml`, `game/deploy.yaml`); the list
and the merge live in `tools/deploy_manifests.py`. Environment variables: `HOSTDRV_DIR` (default `/mnt/c/os32`), `NP21W_DIR` (default `/tmp/np21w`).

Build artifacts land in `build/out/` (gitignored), not the repository root.

## ⚠️ Known Gotchas

**カーネル内 selftest はブート時に必ず走る** (`kernel/kselftest.c`): kstring_asm /
kmalloc / kprintf の境界ケースを実機で毎回検証する。失敗すると赤字で項目名が出る。
`userland/tests/klibc_test.c` は **newlib とリンクされる**ので、あちらが通っても
カーネル側の実装は検証されない。プリミティブを触ったら selftest に項目を足すこと。
結果は `kselftest_pass` / `kselftest_fail` を `emu_read_mem` で読める。

**ABI break after KernelAPI change**: After modifying `KernelAPI` struct, run `make clean` before rebuilding. Stale `.o` files with old struct layout cause silent corruption.

**KAPI にポインタ検証はない (設計上の制約)**: GDT にユーザディスクリプタが
なく、外部プログラムも CPL=0 で実行される。KAPI に渡されたポインタ/サイズは
検証されずカーネルがそのまま使うため、プログラムのバグは即カーネル破壊に
なり得る。リング 3 導入はアーキテクチャ変更 (GDT/TSS/ゲート全面改修) で
2026-08 の信頼性向上 (Phase 1-3) ではスコープ外と判断した。
呼び出し側 (プログラム) が防衛的に書くこと。

**ビットマップフォントは `tools/gen_font16.py` で焼く** (旧 `gen_kcg_font.py` の
置き換え)。カーネルは起動時に `/sys/font/default.kcgfont` を読む
(`kernel.c` の `kcg_load_font`)。**IPAex 系は欧文がプロポーショナル**なので、
'W' (14px) や 'M' (13px) を 8px の ANK セルへ中央寄せすると左右が切られて
W が A に、M が V に見える (2026-08-18 まで実際にそうなっていた)。
gen_font16.py はセルからはみ出す字だけ横に畳んでから焼く。
**縦位置は必ずベースライン基準で置く** (`anchor='ls'`)。字ごとの ink box を
基準にすると 'g' 'j' 'y' や '.' ',' が上下にばらつき、行がガタつく。
縦は絶対に縮めないこと (縮めるとベースラインが合わなくなる)。
`--preview out.png` で焼く前に等倍と3倍を並べた確認画像を出せる。
**等倍で読めるかだけが判断基準** — 拡大像だけ見て決めないこと。
**16x16 の漢字は明朝だと細い横画が飛ぶ**ので本文用はゴシック
(`assets/fonts/ipaexg16.kcgfont`)。
半角 (size15/baseline12) と全角 (size16/baseline13) はベースライン行が
1px ずれるが、これは 16px セルに「漢字の高さ13 + 欧文の descender 4」が
収まらないための妥協。共通ベースラインにすると漢字を size13 まで
落とすことになり、そちらの方が明確に見劣りする (比較検証済み)。

**外部プログラムで漢字を出すには変換表を自分で有効化する**:
カーネルは起動時に `/sys/unicode.bin` を `MEM_UNICODE_TABLE_BASE` (0x4A000) へ
読み込み `utf8_set_jis_table_ready(1)` を呼ぶが、**このフラグは lib/utf8.c の
static 変数**で、外部プログラムは自分の `lib/utf8.o` をリンクするため別の実体に
なる。有効化しないと `unicode_to_jis()` が常に 0 を返し、`kcg_draw_utf8()` は
JIS 0x2222 にフォールバックして**漢字が全部 □ になる** (仮名は
ハードコードされた範囲変換なので出てしまい、原因が分かりにくい)。
表のデータ自体は共有物理メモリにあるので、プログラム側で
`utf8_set_jis_table_ready(1)` を呼べば引ける。ただし**無条件に立てないこと** —
ロード失敗時に 0x4A000 の残骸を変換表として読む。既知の対応
(U+4E9C→0x3021 など) を数点検証してから立てる
(`game/app/main.c` の `enable_kanji_table()` が実例)。

**日本語を表示するプログラムはバッファ幅に注意**: UTF-8 の日本語は 1文字3バイト、
表示幅は半角2桁 (16px)。英語前提の `char buf[64]` は簡単にあふれる。
文字列を切り詰めるときは UTF-8 の途中で切らないこと
(`view_panel.c` の `add_line()` が後続バイト 10xxxxxx を見て戻す実装)。

**物理 0x90000 は自動プレイ観測用メールボックスとして予約** (2026-08-18):
game が毎フレーム状態ブロックを書き (`game/app/view_export.c`)、
ホストが `GET /api/mem?addr=0x90000&space=phys` で読む。空き領域
0x8C000-0x9EFFF の一部。V86 セッションはこの領域を壊すが、ゲームと
V86 は同時に使わない。レイアウトを変えたら
`tools/autoplay/driver.py` の `read_mailbox()` と EXPORT_VERSION も更新すること。
ローカルAI (flm serve の gemma4-it:e4b) にゲームを自動プレイさせる
基盤は `tools/autoplay/` (driver.py / flm_serve.py / mcp_server.py)。

**deploy.yaml に無いバイナリは NHD 上で stale 化する**: KAPI レイアウトが
変わると旧バイナリの KAPI 呼び出しが別関数へ飛び、exit 後の `jmp $` で
永久スピンして rshell ごと沈黙する (2026-08-13 に sndctl で実測)。
起動対象になるバイナリは PKG 配布でも必ず deploy.yaml に載せる。

**HostDrv deploy does not override `/usr/bin`**: `make deploy` only writes to `C:\os32`
(the guest's `/host`). PATH resolution prefers the NHD's `/usr/bin`, so running a program
by name after a HostDrv-only deploy silently executes the **old** binary on the NHD.
Library or test changes must be verified with a full NHD deploy
(stop NP21/W → `make deploy-kernel` → restart).

**SQLite MEMSYS5 is a fixed 384KB pool** (`lib/sqlite3/os32_sqlite_vfs.c`): shared by every
DB connection including the kernel-side FEP dictionary. Programs that hold several
connections open at once exhaust it and get `-2` from `db_query`. Engine libraries avoid
this by closing the connection at the end of `*_init()`. 200KB では game
(econ 常時接続 + battle/items/rpg/events の順次ロード) が枯渇して最後の
`db_query` が `out of memory` になったため 2026-08-18 に 384KB へ拡大した
(SQLite 拡張域 0x200000-0x2FFFFF 内、残り ~280KB)。**枯渇の診断は
`db_last_error()` を必ず出すこと** — 戻り値だけでは「テーブルがない」と
区別できず、原因究明が遠回りになる。実機で任意 DB を調べる診断ツール
`dbq` (`userland/tests/dbq.c`) を rshell から使える。

**`mui_pump_input()` consumes the keyboard queue** (`userland/lib/ui/libos32ui_core.c`):
it calls `kbd_trygetchar()` internally to feed microUI, so an app that also polls
`kbd_trygetchar()` in the same frame gets nothing and appears to ignore all key input.
Apps needing their own key handling must read the char once and pass it to
`mui_pump_input_ch(ctx, ch)` instead. (This silently broke every keyboard shortcut in
`game/app` until 2026-08-17 — the auto-play debug timers existed to work
around it.)

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
├── userland/         — User space (built against the SDK)
│   ├── shell/        — System shell (modular, resident at 0x300000)
│   ├── cmds/         — CLI commands (grep, less, sort, diff, find, wc, etc.)
│   ├── system/       — System utilities (hsync, install, cdinst, sndctl)
│   ├── tests/        — Test/demo programs (40+)
│   ├── rust/         — Rust programs (Cargo workspace)
│   ├── lib/          — User-space libraries (gfx, math, md, mgx, ui, save, ...)
│   └── deploy.yaml   — This layer's deployment manifest
├── apps/             — Standard applications (edit, mdview, mgxview, ui_demo, ...)
│                       Built from the SDK alone; no dependency on the OS tree
├── game/             — The game (app/, lib/, assets/, data/) — SDK build too
├── sdk/              — Distributable SDK (headers, crt, linker scripts, rust, example)
├── tools/            — Host-side tools (Python scripts, kapi.json)
├── docs/             — Documentation (specs, policies, tasks, manpages)
├── build/            — Build config (Makefiles, linker scripts)
│   └── out/          — Build artifacts (kernel.bin, sqlite.bin, vmkernel.lz4,
│                       unicode.bin, kernel.elf, kernel.map) — gitignored
├── assets/           — Game assets
├── CLAUDE.md         — This file (AI guidance)
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
