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
0x400000–         External program band: code+bss, then newlib sbrk, guard page,
                  KAPI exec_heap directly below the stack guard (no fixed 1MB cap
                  since 2026-09-04; sbrk/exec_heap split by OS32X heap_size or 50/50)
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
| `userland/lib/` | User-space libraries (see below) |

#### Shell Architecture (`userland/shell/`)

Modular design with command registration mechanism (`ShellCmd` struct, max 128 commands):
- `main.c` — Entry, command router, pipe execution, wildcard expansion
- `ui.c` — Main loop, line editing, tab completion, command history
- `cmd_base.c` / `cmd_file.c` / `cmd_dir.c` / `cmd_mnt.c` / `cmd_sys.c` / `cmd_env.c` — Command modules
- `cmd_script.c` — Script engine (if/else/for/while/source, max 128 lines, 4-deep nesting)
- `rshell.c` — Remote shell via serial

#### Libraries (`userland/lib/`)

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

**Game Engine** (別リポジトリ `ske-studio/os32-game` へ移動):

> ai / battle / board / chem / econ / event / inv / map / rpg / text / turn の
> 11 ライブラリは os32-game が自前のソースからビルドする。いずれも `*_init()` で
> マスターデータを RAM キャッシュへ読み込んだら **DB 接続を即閉じる** 作法。
> 設計は os32-game リポジトリの `docs/` を参照。

### Graphics

All pixel writes go through CPU directly to VRAM at `0xA8000` (graphics planes) — see **[HW1]**. Flow: draw to backbuffer → `gfx_present()` → VRAM with page flip (port A4h/A6h). Page flip is automatic; no VSYNC wait needed.

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

**カーネル内 selftest はブート時に必ず走る** (`kernel/kselftest.c`): kstring_asm /
kmalloc / kprintf の境界ケースを実機で毎回検証する。失敗すると赤字で項目名が出る。
`userland/tests/klibc_test.c` は **newlib とリンクされる**ので、あちらが通っても
カーネル側の実装は検証されない。プリミティブを触ったら selftest に項目を足すこと。
結果は `kselftest_pass` / `kselftest_fail` を `emu_read_mem` で読める。

**ABI break after KernelAPI change**: After modifying `KernelAPI` struct, run `make clean` before rebuilding. Stale `.o` files with old struct layout cause silent corruption.

**KAPI ポインタ検証とリング3 (v2 M1-M3 で解消済み, 2026-09-03)**: 外部プログラムは
**既定で CPL=3 (リング3)** で走る。GDT に USER セグメント (USER_CS=0x23/USER_DS=0x2B)、
プログラムごとに独立 PD (カーネル帯域は全 PD 共有・非 USER)、KAPI は USER
トランポリンページ経由で `int 0x80` に入る。ディスパッチャがポインタ引数を
アプリ帯 (0x400000-0x7FFFFF)/SHM/VRAM の範囲で早期検証し、可変長引数など
検証しきれない不正アクセスは「ring3 syscall 実行中フォールトガード」が捕捉して、
**不正ポインタはアプリを kill するだけ (カーネルは無傷、`fault_kill_count` を増やして
シェル復帰)**。かつての「KAPI にポインタ検証はない/呼ぶ側が防衛的に」という
設計制約 (旧 ABI4) はアーキテクチャで解消し、CONSTRAINTS.md の規則から削除した。
**例外**: shell (常駐 0x300000, CPL=0, 信頼扱い) と `OS32X_FLAG_FORCE_CPL0`
(mkos32x `--cpl0`) で明示的に CPL=0 起動したプログラムのみ、従来どおり非保護。
設計と検証は `docs/tasks/v2/`。

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
(実例は `ske-studio/os32-game` の `app/main.c` の `enable_kanji_table()`)。

**日本語を表示するプログラムはバッファ幅に注意**: UTF-8 の日本語は 1文字3バイト、
表示幅は半角2桁 (16px)。英語前提の `char buf[64]` は簡単にあふれる。
文字列を切り詰めるときは UTF-8 の途中で切らないこと
(`view_panel.c` の `add_line()` が後続バイト 10xxxxxx を見て戻す実装)。

**物理 0x90000 は自動プレイ観測用メールボックスとして予約** (2026-08-18):
game が毎フレーム状態ブロックを書き (`os32-game` の `app/view_export.c`)、
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
2026-09-04 から `make deploy` / `deploy-nhd` / `deploy-kernel` は同期のあとに
`tools/prune_stale.py` でマニフェストに無いシステム側の *.bin を削除する
(`NO_PRUNE=1` で一覧のみ、`make prune-stale` で手動確認)。

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
`os32-game` の app until 2026-08-17 — the auto-play debug timers existed to work
around it.)

**`exec_exit()` reclaims FDs / redirects / pipe buffers by owner** (`exec/exec.c`):
every open FD, std-FD redirect and pipe buffer is tagged at allocation with
`res_owner_get()` (= exec nest level: 0 kernel, 1 shell, 2+ apps) and `exec_exit()`
only reclaims what the exiting level allocated. Kernel-resident FDs (e.g. the FEP
dictionary's SQLite connection) are owner 0 and additionally protected with
`vfs_fd_set_protect(fd, 1)`; before the owner tags existed (until 2026-09-03) *every*
FD ≥ 3 was closed on any program exit, which caused the 2026-08-07 "FEP zero
candidates" bug, and every pipe buffer was kfree'd, which made `ext_cmd1 | ext_cmd2`
hang on keyboard input. When returning to the parent, restore the parent's exec_heap
with `exec_heap_restore_state()` — never `exec_heap_init_at()`, which rewrites the
parent's first block header and turns later frees into
`[exec_heap] bad magic feeefeee (double free?)`.

**The shell has two heaps that must not overlap** (`include/memmap.h`): newlib's sbrk
(malloc / stdio buffers) grows from the BSS end up to `MEM_SHELL_GUARD`, and the KAPI
`mem_alloc` exec_heap lives at `MEM_SHELL_HEAP_BASE` (0x380000, 512KB). Both used to
start at the BSS end and overwrote each other (`ls > file` garbage,
`pipe: out of memory`, double-free warnings). `kernel/paging.c` must keep 0x380000–
0x3FFFFF present (it used to be an NP gap).

**Never touch the FS from inside a `sys_ls` callback path without a private buffer**
(`fs/ext2_dir.c`): `ext2_list_dir` copies each directory block to a local buffer before
invoking callbacks, because a callback that writes a file (e.g. `ls > file` — printf
goes through `ext2_write_stream`) clobbers the shared `ext2_g_aux` block buffer.
FatFs/HostDrv list_dir have not been audited for the same re-entrancy.

**VFS error codes are `OS32_ERR_*` in `os32_kapi_shared.h`** (SSoT); `VFS_ERR_*` are
aliases. FS drivers must translate their internal codes at the boundary
(`ext2_to_vfs_err`) — ext2's `-3` is NOTFOUND while the VFS `-3` is NOMOUNT, and until
2026-09-03 raw ext2 codes leaked to the shell. `vfs_open` refuses directories
(`OS32_ERR_ISDIR`) and `vfs_chdir` refuses anything that is not an existing directory.

**NHD デプロイの作業イメージは `build/nhd/os32.nhd`** (2026-09-04 に `/tmp/os32.nhd` から移動):
`/tmp` は WSL 再起動で消え、消えた状態の `make deploy-nhd` は **NHD を書かずに exit 0** で
返っていた (nhd_deploy.py が失敗を終了コードに載せていなかった)。同日に修正済み:
無ければ Windows 側から自動 pull し (NP21/W 停止中のみ可能)、失敗は exit 1、さらに
`os32-cycle deploy` はブート後にゲストの `/boot/vmkernel.lz4` サイズが手元の成果物と
一致しなければ FAIL にする。**「配備完了」の文言だけを信じないこと** — カーネル変更の
検証は `kselftest_pass` を新しい kernel.map のアドレスで読むか、この一致チェックで行う。

**テキスト GDC のカーソルは CSRFORM (0x4B) の DC ビットでしか出ない**: 旧 `GDC_CMD_CSON`
(0x0B) は uPD7220 に存在しないコマンドで、OS32 は 2026-09-04 までハードウェアカーソルを
一度も表示していなかった。V86 中に DOS が DC=1 にしたカーソルだけが終了後も DOS 最後の
位置で点滅し続け「左下でちかちか」に見えた。現在は `console_hw_cursor_enable()` (起動時 /
V86 終了時 / `console_set_cursor`) で表示にし、`console_hw_cursor_sync()` が文字列出力の
末尾で論理位置へ追従させる。GDC の実状態は MCP `emu_gdc` の `m_csrform` で読める
(`8f0e7b` = 表示・2 ライン下線)。

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
├── sdk/              — Distributable SDK (headers, crt, linker scripts, rust, example)
├── tools/            — Host-side tools (Python scripts, kapi.json)
├── docs/             — Documentation (specs, policies, tasks, manpages)
├── build/            — Build config (Makefiles, linker scripts)
│   └── out/          — Build artifacts (kernel.bin, sqlite.bin, vmkernel.lz4,
│                       unicode.bin, kernel.elf, kernel.map) — gitignored
├── assets/           — Assets (fonts, FEP dict, unicode table, etc.)
├── CLAUDE.md         — This file (AI guidance)
└── Makefile          — Top-level build script
```

## Documentation

| Document | Content |
|----------|---------|
| `docs/INDEX.md` | Document index (master reference) |
| `docs/01_system.md` ~ `docs/10_notes.md` | Kernel technical spec (10 parts) |
| `docs/KAPI_SPEC.md` | KernelAPI spec (current: **v40**, 171 functions + 2 data fields) |
| `docs/DEVELOPMENT.md` | Architecture, subsystem details |
| `docs/POLICY_DEV.md` | Coding policy, build rules |
| `docs/POLICY_DEBUG.md` | Debug procedures, binary deployment checklist |
| `docs/ROADMAP.md` | Release roadmap (v1.1 GUI shell~) |
| `docs/tasks/fep/` | FEP (Japanese input) detailed design, P1–P7 |
| `docs/tasks/game/` | Board-game RPG port plan + engine extension plan |
| `docs/tasks/wintree_port/` | Record of the feat/vdm work-tree port |
| `/home/hight/np21w-src/docs/` | **NP21/W ai-debug fork** (AI-native emulator debugging: embedded HTTP debug server + MCP). Plan, build setup, architecture. WSL repo is the source of truth; build and deploy from WSL with `make build && make deploy` (mirrors to Windows and drives MSBuild) |

For PC-9800 hardware specs, read the local mirror `docs/hw/` (`PC9800Bible/` = PC-9801
Bible, `undocumented/` = UNDOCUMENTED 9801/9821 Vol.2 `io_*.md`). The source of truth is
`C:\WATCOM\docs\` (`/mnt/c/WATCOM/docs/`); refresh the mirror with `tools/sync_hwdocs.sh`.
**The mirror is copyrighted material and is gitignored (`/docs/hw/`) — never commit it.**
When the two books disagree, UNDOCUMENTED wins (see `docs/INDEX.md`).
