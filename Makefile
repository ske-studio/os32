# PC-9801 OS32 Makefile for GCC/NASM
#
# Phase 3 改修: モジュール別インクルードパス
# - カーネル共通: include/ のみ (types.h, io.h, console.h, tvram.h, os32_kapi_shared.h)
# - 各モジュール: 必要なサブディレクトリのみを -I で指定
# - 外部プログラム: include/ と programs/ のみ

.DEFAULT_GOAL := all

# 環境変数を .env ファイルから読み込み (存在する場合)
-include .env

# 環境変数 (デプロイ先)
NP21W_DIR ?= /tmp/np21w

# クロスコンパイラパス
CROSS_DIR ?= /usr/local/cross

# Directories
PROJDIR = .

# Tools
CC = i386-elf-gcc
AS = nasm
LD = i386-elf-ld
OBJCOPY = i386-elf-objcopy

# === インクルードパス (モジュール別) ===
# 共通: 全カーネルモジュールが参照する基盤ヘッダ
INC_COMMON = -I. -Iinclude

# カーネルコア: 自身 + ドライバ + fs + exec + shell + gfx + lib + kapi
# (kernel.c は全サブシステムの初期化を行うため全モジュールを参照)
INC_KERNEL = $(INC_COMMON) -Ikernel -Idrivers -Ifs -Iexec -Igfx -Ilib -Ikapi -Ilib/sqlite3

# ドライバ: 共通 + 自身 + gfx (kcg->gfx依存)
INC_DRIVERS = $(INC_COMMON) -Idrivers -Igfx -Ilib

# GFX: 共通 + 自身 + ドライバ (palette依存) + FS (dump時のファイル出力)
INC_GFX = $(INC_COMMON) -Igfx -Idrivers -Ifs -Ilib -Ikernel

# FS: 共通 + 自身 + ドライバ (disk/ide依存)
INC_FS = $(INC_COMMON) -Ifs -Idrivers -Ikernel -Ilib


# exec: 共通 + exec + kapi + fs + gfx + ドライバ (kbd依存)
INC_EXEC = $(INC_COMMON) -Iexec -Ikapi -Ifs -Igfx -Idrivers -Ilib -Ikernel

# KAPI: 全モジュール (全APIラッパーのため)
INC_KAPI = $(INC_COMMON) -Ikapi -Ikernel -Idrivers -Ifs -Iexec -Igfx -Ilib -Ilib/sqlite3

# lib: 共通 + 自身 (汎用ライブラリ: カーネル依存なし)
INC_LIB = $(INC_COMMON) -Ilib

# === コンパイルフラグ ===
CFLAGS_BASE = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O2 -Wall -fcommon
# SQLite専用フラグ: -Os (サイズ最適化, -O0のスタック肥大化回避) + -Wno-long-long (int64リテラル)
CFLAGS_SQLITE = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -Os -fcommon -ffunction-sections -fdata-sections -Wno-long-long -w -DNDEBUG
LDFLAGS = -m elf_i386 -T build/os32.ld -Map=kernel.map -nostdlib --nmagic --gc-sections \
	-L$(shell $(CC) -print-libgcc-file-name | xargs dirname)

# SQLite: カーネルFS/ドライバ + SQLiteヘッダ (VFS実装用)
INC_SQLITE = $(INC_COMMON) -Ilib/sqlite3 -Ifs -Idrivers -Ilib -Ikernel

ASM_STANDALONE = boot/boot_fat.asm boot/loader_fat.asm boot/boot_hdd.asm boot/loader_hdd.asm
BIN_STANDALONE = $(ASM_STANDALONE:.asm=.bin)

ASM_KERNEL = kernel/kentry.asm kernel/isr_stub.asm kernel/setjmp.asm lib/kstring_asm.asm lib/sqlite3/sqlite_stack.asm
ASM_KERNEL_OBJ = $(ASM_KERNEL:.asm=.o)

C_KERNEL = \
    kernel/kernel.c kernel/boot_splash.c kernel/idt.c kernel/isr_handlers.c kernel/cpu_calibrate.c \
    kernel/paging.c kernel/pgalloc.c kernel/shm.c kernel/kmalloc.c kernel/console.c kernel/sys.c \
    kernel/ime.c kernel/ime_romkana.c kernel/ime_dict.c kernel/snd_engine.c \
    drivers/kbd.c drivers/serial.c drivers/fm.c \
    drivers/fdc.c drivers/disk.c drivers/ide.c drivers/atapi.c drivers/rtc.c drivers/dev.c drivers/kcg.c drivers/np2sysp.c \
    drivers/mouse.c drivers/mouse_bus.c drivers/mouse_seamless.c \
    gfx/gfx_core.c gfx/gfx_vram.c gfx/gfx_scroll.c gfx/palette.c \
    fs/fat12.c fs/ext2_super.c fs/ext2_inode.c fs/ext2_dir.c fs/ext2_file.c fs/ext2_fmt.c fs/ext2_vfs.c fs/vfs.c fs/vfs_fd.c fs/fd_redirect.c fs/pipe_buffer.c fs/iso9660.c fs/hostdrvfs.c \
    exec/exec.c exec/exec_heap.c \
    kapi/kapi_generated.c kapi/kapi_db.c \
    lib/path.c lib/utf8.c lib/kprintf.c lib/lzss.c lib/lz4.c lib/os_time.c lib/kstring.c lib/kutf16.c lib/kmath.c

# SQLite関連 (カーネル拡張域 0x18A000 に配置)
C_SQLITE = lib/sqlite3/sqlite3.c lib/sqlite3/os32_sqlite_vfs.c lib/sqlite3/os32_sqlite_test.c
C_SQLITE_OBJ = $(C_SQLITE:.c=.o)

C_KERNEL_OBJ = $(C_KERNEL:.c=.o)

# Programs
PROGRAM_FLAGS = $(CFLAGS_BASE) -I. -Iinclude -Iprograms -Iprograms/shell -Iprograms/libos32gfx -Iprograms/libos32math -Iprograms/libos32chem -Iprograms/libos32map -Iprograms/libos32input -Iprograms/libos32asset -Iprograms/libos32ecs -Iprograms/libos32text -Iprograms/libos32econ -Iprograms/libos32ai -Iprograms/libos32battle -Iprograms/libos32board -Iprograms/libos32event -Iprograms/libos32inv -Iprograms/libpyxel -Iprograms/libtilemap -I$(CROSS_DIR)/i386-elf/include
PROGRAM_LDFLAGS = -m elf_i386 -T build/app.ld -nostdlib --nmagic --gc-sections \
	-L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0

CRT0_OBJ = programs/crt0.o programs/crt0_c.o programs/libos32/syscalls.o programs/libos32/help.o
DBG_OBJ  = programs/libos32/dbgserial.o

C_CMDS    = $(wildcard programs/cmds/*.c)
C_APPS    = $(filter-out programs/apps/edit.c, $(wildcard programs/apps/*.c))
C_TESTS   = $(filter-out programs/tests/skk_test.c programs/tests/fep_test.c programs/tests/pyxel_test.c programs/tests/gfx200_test.c programs/tests/gfx_demo200.c programs/tests/blit_test.c programs/tests/blit_test2.c programs/tests/demo_tile.c programs/tests/tile_bench.c programs/tests/rotate_test.c programs/tests/db_test.c programs/tests/e2test.c programs/tests/math_test.c programs/tests/chem_test.c programs/tests/chem_demo.c programs/tests/map_test.c programs/tests/map_demo.c programs/tests/input_test.c programs/tests/asset_test.c programs/tests/asset_demo.c programs/tests/ecs_test.c programs/tests/ecs_demo.c programs/tests/text_test.c programs/tests/text_demo.c programs/tests/econ_test.c programs/tests/ai_test.c programs/tests/btl_test.c programs/tests/board_test.c programs/tests/evt_test.c programs/tests/inv_test.c, $(wildcard programs/tests/*.c))
C_SYSTEM  = $(filter-out programs/system/lzss.c programs/system/lz4.c programs/system/cdinst.c, $(wildcard programs/system/*.c))

C_BASE_PROGRAMS = $(C_CMDS) $(C_APPS) $(C_TESTS) $(C_SYSTEM)
BASE_PROGRAMS_BIN = $(C_BASE_PROGRAMS:.c=.bin) programs/shell.bin

# === Shell Module ===
SHELL_SRC = $(wildcard programs/shell/*.c)
SHELL_OBJ = $(SHELL_SRC:.c=.o)

programs/shell/%.o: programs/shell/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libfiler -c $< -o $@

# filer_draw は shell が参照するため SHELL_OBJ に含める
FILER_DRAW_OBJ = programs/libfiler/filer_draw.o

programs/shell.elf: build/app_sys.ld $(CRT0_OBJ) $(SHELL_OBJ) $(FILER_DRAW_OBJ)
	$(LD) -m elf_i386 -T build/app_sys.ld -nostdlib --nmagic --gc-sections -L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0 -o $@ $(CRT0_OBJ) $(SHELL_OBJ) $(FILER_DRAW_OBJ) -lc -lgcc

# === Edit (VZ-inspired Editor) Module ===
EDIT_SRC = $(wildcard programs/apps/edit/*.c)
EDIT_OBJ = $(EDIT_SRC:.c=.o)

programs/apps/edit/%.o: programs/apps/edit/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/apps/edit -c $< -o $@

programs/apps/edit.elf: build/app.ld $(CRT0_OBJ) $(EDIT_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(EDIT_OBJ) $(GFX_OBJ) -lc -lgcc

# === SKK Module ===
SKK_SRC = $(wildcard programs/skk/*.c)
SKK_OBJ = $(SKK_SRC:.c=.o)

programs/skk/%.o: programs/skk/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/skk_test.o: programs/tests/skk_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/skk_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/skk_test.o $(SKK_OBJ) lib/utf8.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/skk_test.o $(SKK_OBJ) lib/utf8.o -lc -lgcc


# === LZSS Command ===
# lib/lzss.c を外部プログラム用に再コンパイル
lib/lzss_prog.o: lib/lzss.c lib/lzss.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/system/lzss.o: programs/system/lzss.c
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/system/lzss.elf: build/app.ld $(CRT0_OBJ) programs/system/lzss.o lib/lzss_prog.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/system/lzss.o lib/lzss_prog.o -lc -lgcc

lzss_cmd: $(CRT0_OBJ) programs/system/lzss.bin

# === LZ4 Command ===
# lib/lz4.c を外部プログラム用に再コンパイル
lib/lz4_prog.o: lib/lz4.c lib/lz4.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/system/lz4.o: programs/system/lz4.c
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/system/lz4.elf: build/app.ld $(CRT0_OBJ) programs/system/lz4.o lib/lz4_prog.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/system/lz4.o lib/lz4_prog.o -lc -lgcc

lz4_cmd: $(CRT0_OBJ) programs/system/lz4.bin

# === CD Installer (cdinst) ===
programs/libos32/pkg.o: programs/libos32/pkg.c programs/libos32/pkg.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/system/cdinst.o: programs/system/cdinst.c programs/libos32/pkg.h programs/libos32/dbgserial.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/system/cdinst.elf: build/app.ld $(CRT0_OBJ) programs/system/cdinst.o programs/libos32/pkg.o $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/system/cdinst.o programs/libos32/pkg.o $(DBG_OBJ) -lc -lgcc

cdinst: $(CRT0_OBJ) programs/system/cdinst.bin

# === libos32math Module (整数数学ライブラリ — 最も基底のライブラリ) ===
LIBMATH_SRC = $(wildcard programs/libos32math/*.c)
LIBMATH_OBJ = $(LIBMATH_SRC:.c=.o)

programs/libos32math/%.o: programs/libos32math/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === OS32GFX Module ===
GFX_SRC = $(wildcard programs/libos32gfx/*.c) \
          $(wildcard programs/libos32gfx/draw/*.c) \
          $(wildcard programs/libos32gfx/text/*.c) \
          $(wildcard programs/libos32gfx/geom/*.c)
ASM_GFX_SRC = $(wildcard programs/libos32gfx/asm/*.asm)
ASM_GFX_OBJ = $(ASM_GFX_SRC:.asm=.o)
GFX_OBJ = $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ) $(LIBMATH_OBJ) lib/utf8.o

programs/libos32gfx/%.o: programs/libos32gfx/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32gfx/draw/%.o: programs/libos32gfx/draw/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32gfx/text/%.o: programs/libos32gfx/text/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32gfx/geom/%.o: programs/libos32gfx/geom/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32gfx/asm/%.o: programs/libos32gfx/asm/%.asm programs/libos32gfx/asm/gfx_const.inc
	$(AS) -f elf32 -Iprograms/libos32gfx/asm/ $< -o $@

# === Bench Module ===
BENCH_SRC = $(wildcard programs/tests/bench/*.c)
BENCH_OBJ = $(BENCH_SRC:.c=.o)

programs/tests/bench/%.o: programs/tests/bench/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/bench.elf: build/app.ld $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ) -lc -lgcc

bench: $(CRT0_OBJ) programs/tests/bench.bin

# === libpyxel Module (Pyxel互換ゲームエンジン) ===
PYXEL_SRC = $(wildcard programs/libpyxel/*.c)
PYXEL_OBJ = $(PYXEL_SRC:.c=.o)

programs/libpyxel/%.o: programs/libpyxel/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === libtilemap Module (タイルマップエンジン) ===
TILEMAP_SRC = $(wildcard programs/libtilemap/*.c)
TILEMAP_ASM_SRC = $(wildcard programs/libtilemap/*.asm)
TILEMAP_ASM_OBJ = $(TILEMAP_ASM_SRC:.asm=.o)
TILEMAP_OBJ = $(TILEMAP_SRC:.c=.o) $(TILEMAP_ASM_OBJ)

programs/libtilemap/%.o: programs/libtilemap/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libtilemap/%.o: programs/libtilemap/%.asm
	$(AS) -f elf32 $< -o $@

# === Bench Scale2x Module (libpyxel Phase 0) ===
BENCH_S2X_SRC = programs/tests/bench_scale2x/main.c
BENCH_S2X_OBJ = $(BENCH_S2X_SRC:.c=.o)

programs/tests/bench_scale2x/%.o: programs/tests/bench_scale2x/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/bench_scale2x.elf: build/app.ld $(CRT0_OBJ) $(BENCH_S2X_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_S2X_OBJ) $(GFX_OBJ) -lc -lgcc

bench_scale2x: $(CRT0_OBJ) programs/tests/bench_scale2x.bin

# === Pyxel Infrastructure Test ===
programs/tests/pyxel_test.o: programs/tests/pyxel_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/pyxel_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/pyxel_test.o $(PYXEL_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/pyxel_test.o $(PYXEL_OBJ) $(GFX_OBJ) -lc -lgcc

pyxel_test: $(CRT0_OBJ) programs/tests/pyxel_test.bin

# === GFX 200-Line Mode Test ===
programs/tests/gfx200_test.o: programs/tests/gfx200_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/gfx200_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/gfx200_test.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/gfx200_test.o $(GFX_OBJ) -lc -lgcc

gfx200_test: $(CRT0_OBJ) programs/tests/gfx200_test.bin

# === GFX 200-Line Demo ===
programs/tests/gfx_demo200.o: programs/tests/gfx_demo200.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/gfx_demo200.elf: build/app.ld $(CRT0_OBJ) programs/tests/gfx_demo200.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/gfx_demo200.o $(GFX_OBJ) -lc -lgcc

gfx_demo200: $(CRT0_OBJ) programs/tests/gfx_demo200.bin

# === Blit Transparent Test ===
programs/tests/blit_test.o: programs/tests/blit_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/blit_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/blit_test.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/blit_test.o $(GFX_OBJ) -lc -lgcc

blit_test: $(CRT0_OBJ) programs/tests/blit_test.bin

# === Blit Transparent Visual Test ===
programs/tests/blit_test2.o: programs/tests/blit_test2.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/blit_test2.elf: build/app.ld $(CRT0_OBJ) programs/tests/blit_test2.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/blit_test2.o $(GFX_OBJ) -lc -lgcc

blit_test2: $(CRT0_OBJ) programs/tests/blit_test2.bin

# === Rotate Blit Test ===
programs/tests/rotate_test.o: programs/tests/rotate_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/rotate_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/rotate_test.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/rotate_test.o $(GFX_OBJ) -lc -lgcc

rotate_test: $(CRT0_OBJ) programs/tests/rotate_test.bin

# === Tilemap Demo ===
programs/tests/demo_tile.o: programs/tests/demo_tile.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/demo_tile.elf: build/app.ld $(CRT0_OBJ) programs/tests/demo_tile.o $(TILEMAP_OBJ) $(GFX_OBJ) $(LIBASSET_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/demo_tile.o $(TILEMAP_OBJ) $(GFX_OBJ) $(LIBASSET_OBJ) -lc -lgcc

demo_tile: $(CRT0_OBJ) programs/tests/demo_tile.bin

# === Tilemap Benchmark ===
programs/tests/tile_bench.o: programs/tests/tile_bench.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/tile_bench.elf: build/app.ld $(CRT0_OBJ) programs/tests/tile_bench.o $(TILEMAP_OBJ) $(GFX_OBJ) $(LIBASSET_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/tile_bench.o $(TILEMAP_OBJ) $(GFX_OBJ) $(LIBASSET_OBJ) -lc -lgcc

tile_bench: $(CRT0_OBJ) programs/tests/tile_bench.bin

# === libos32db Module (SQLite ユーザー空間ライブラリ) ===
LIBOS32DB_SRC = programs/libos32db/libos32db.c
LIBOS32DB_OBJ = $(LIBOS32DB_SRC:.c=.o)

programs/libos32db/%.o: programs/libos32db/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === DB Test ===
programs/tests/db_test.o: programs/tests/db_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/db_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/db_test.o $(LIBOS32DB_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/db_test.o $(LIBOS32DB_OBJ) -lc -lgcc

db_test: $(CRT0_OBJ) programs/tests/db_test.bin

# === SQLite Standalone Test (ユーザー空間で直接リンク) ===
SQLITE_SA_DIR = programs/tests/sqlite_standalone
SQLITE_SA_CFLAGS = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O1 -fcommon -Wno-long-long -w -I. -Iinclude -Iprograms -Ilib/sqlite3 -include lib/sqlite3/os32_sqlite_config.h -I$(CROSS_DIR)/i386-elf/include

$(SQLITE_SA_DIR)/sqlite3_user.o: lib/sqlite3/sqlite3.c lib/sqlite3/os32_sqlite_config.h
	$(CC) $(SQLITE_SA_CFLAGS) -c $< -o $@

$(SQLITE_SA_DIR)/sqlite_user_vfs.o: $(SQLITE_SA_DIR)/sqlite_user_vfs.c lib/sqlite3/os32_sqlite_config.h
	$(CC) $(SQLITE_SA_CFLAGS) -c $< -o $@

$(SQLITE_SA_DIR)/sqlite_standalone.o: $(SQLITE_SA_DIR)/sqlite_standalone.c lib/sqlite3/os32_sqlite_config.h
	$(CC) $(SQLITE_SA_CFLAGS) -c $< -o $@

SQLITE_SA_OBJ = $(CRT0_OBJ) $(DBG_OBJ) \
	$(SQLITE_SA_DIR)/sqlite_standalone.o \
	$(SQLITE_SA_DIR)/sqlite_user_vfs.o \
	$(SQLITE_SA_DIR)/sqlite3_user.o

$(SQLITE_SA_DIR)/sqlite_standalone.elf: build/app.ld $(SQLITE_SA_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(SQLITE_SA_OBJ) -lc -lgcc

sqlite_standalone: $(CRT0_OBJ) $(SQLITE_SA_DIR)/sqlite_standalone.bin

# === ext2 DIND Write Test ===
programs/tests/e2test.o: programs/tests/e2test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/e2test.elf: build/app.ld $(CRT0_OBJ) programs/tests/e2test.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/e2test.o -lc -lgcc

e2test: $(CRT0_OBJ) programs/tests/e2test.bin

# === Math Test (libos32math テスト) ===
programs/tests/math_test.o: programs/tests/math_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/math_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/math_test.o $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/math_test.o $(LIBMATH_OBJ) -lc -lgcc

math_test: $(CRT0_OBJ) programs/tests/math_test.bin

# === libos32chem Module (化学エンジンライブラリ) ===
LIBCHEM_SRC = $(wildcard programs/libos32chem/*.c)
LIBCHEM_OBJ = $(LIBCHEM_SRC:.c=.o)

programs/libos32chem/%.o: programs/libos32chem/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Chem Test (libos32chem テスト) ===
programs/tests/chem_test.o: programs/tests/chem_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/chem_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/chem_test.o $(LIBCHEM_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/chem_test.o $(LIBCHEM_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ) -lc -lgcc

chem_test: $(CRT0_OBJ) programs/tests/chem_test.bin

# === Chem Demo (化学エンジンデモ) ===
programs/tests/chem_demo.o: programs/tests/chem_demo.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/chem_demo.elf: build/app.ld $(CRT0_OBJ) programs/tests/chem_demo.o $(LIBCHEM_OBJ) $(LIBOS32DB_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/chem_demo.o $(LIBCHEM_OBJ) $(LIBOS32DB_OBJ) $(GFX_OBJ) -lc -lgcc

chem_demo: $(CRT0_OBJ) programs/tests/chem_demo.bin

# === libos32map Module (マップ管理ライブラリ) ===
LIBMAP_SRC = $(wildcard programs/libos32map/*.c)
LIBMAP_OBJ = $(LIBMAP_SRC:.c=.o)

programs/libos32map/%.o: programs/libos32map/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Map Test (libos32map テスト) ===
programs/tests/map_test.o: programs/tests/map_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/map_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/map_test.o $(LIBMAP_OBJ) $(LIBOS32DB_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/map_test.o $(LIBMAP_OBJ) $(LIBOS32DB_OBJ) -lc -lgcc

map_test: $(CRT0_OBJ) programs/tests/map_test.bin

# === Map Demo (libos32map + libtilemap 統合デモ) ===
programs/tests/map_demo.o: programs/tests/map_demo.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/map_demo.elf: build/app.ld $(CRT0_OBJ) programs/tests/map_demo.o $(LIBMAP_OBJ) $(LIBOS32DB_OBJ) $(TILEMAP_OBJ) $(GFX_OBJ) $(LIBASSET_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/map_demo.o $(LIBMAP_OBJ) $(LIBOS32DB_OBJ) $(TILEMAP_OBJ) $(GFX_OBJ) $(LIBASSET_OBJ) -lc -lgcc

map_demo: $(CRT0_OBJ) programs/tests/map_demo.bin

# === libos32input Module (入力抽象化ライブラリ) ===
LIBINPUT_SRC = $(wildcard programs/libos32input/*.c)
LIBINPUT_OBJ = $(LIBINPUT_SRC:.c=.o)

programs/libos32input/%.o: programs/libos32input/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === Input Test (libos32input テスト) ===
programs/tests/input_test.o: programs/tests/input_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/input_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/input_test.o $(LIBINPUT_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/input_test.o $(LIBINPUT_OBJ) $(LIBMATH_OBJ) -lc -lgcc

input_test: $(CRT0_OBJ) programs/tests/input_test.bin

# === libos32asset Module (アセット・リソース管理ライブラリ) ===
LIBASSET_SRC = $(wildcard programs/libos32asset/*.c)
LIBASSET_OBJ = $(LIBASSET_SRC:.c=.o)

programs/libos32asset/%.o: programs/libos32asset/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === Asset Test (libos32asset テスト) ===
programs/tests/asset_test.o: programs/tests/asset_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/asset_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/asset_test.o $(LIBASSET_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/asset_test.o $(LIBASSET_OBJ) -lc -lgcc

asset_test: $(CRT0_OBJ) programs/tests/asset_test.bin

# === Asset Demo (非同期ロード デモ) ===
programs/tests/asset_demo.o: programs/tests/asset_demo.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/asset_demo.elf: build/app.ld $(CRT0_OBJ) programs/tests/asset_demo.o $(LIBASSET_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/asset_demo.o $(LIBASSET_OBJ) -lc -lgcc

asset_demo: $(CRT0_OBJ) programs/tests/asset_demo.bin

# === libos32ecs Module (ECS ゲームオブジェクト管理ライブラリ) ===
LIBECS_SRC = $(wildcard programs/libos32ecs/*.c)
LIBECS_OBJ = $(LIBECS_SRC:.c=.o)

programs/libos32ecs/%.o: programs/libos32ecs/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === ECS Test (libos32ecs テスト) ===
programs/tests/ecs_test.o: programs/tests/ecs_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/ecs_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/ecs_test.o $(LIBECS_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/ecs_test.o $(LIBECS_OBJ) $(LIBMATH_OBJ) -lc -lgcc

ecs_test: $(CRT0_OBJ) programs/tests/ecs_test.bin

# === ECS Demo (libos32ecs + libos32chem 統合デモ) ===
programs/tests/ecs_demo.o: programs/tests/ecs_demo.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/ecs_demo.elf: build/app.ld $(CRT0_OBJ) programs/tests/ecs_demo.o $(LIBECS_OBJ) $(LIBCHEM_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/ecs_demo.o $(LIBECS_OBJ) $(LIBCHEM_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ) -lc -lgcc

ecs_demo: $(CRT0_OBJ) programs/tests/ecs_demo.bin

# === libos32text Module (テキスト管理ライブラリ) ===
LIBTEXT_SRC = $(wildcard programs/libos32text/*.c)
LIBTEXT_OBJ = $(LIBTEXT_SRC:.c=.o)

programs/libos32text/%.o: programs/libos32text/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Text Test (libos32text テスト) ===
programs/tests/text_test.o: programs/tests/text_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/text_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/text_test.o $(LIBTEXT_OBJ) $(LIBOS32DB_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/text_test.o $(LIBTEXT_OBJ) $(LIBOS32DB_OBJ) -lc -lgcc

text_test: $(CRT0_OBJ) programs/tests/text_test.bin

# === Text Demo (libos32text ビジュアルデモ) ===
programs/tests/text_demo.o: programs/tests/text_demo.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/text_demo.elf: build/app.ld $(CRT0_OBJ) programs/tests/text_demo.o $(LIBTEXT_OBJ) $(LIBOS32DB_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/text_demo.o $(LIBTEXT_OBJ) $(LIBOS32DB_OBJ) $(GFX_OBJ) -lc -lgcc

text_demo: $(CRT0_OBJ) programs/tests/text_demo.bin

# === libos32econ Module (経済エンジンライブラリ) ===
LIBECON_SRC = $(wildcard programs/libos32econ/*.c)
LIBECON_OBJ = $(LIBECON_SRC:.c=.o)

programs/libos32econ/%.o: programs/libos32econ/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Econ Test (libos32econ テスト) ===
programs/tests/econ_test.o: programs/tests/econ_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/econ_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/econ_test.o $(LIBECON_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/econ_test.o $(LIBECON_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ) -lc -lgcc

econ_test: $(CRT0_OBJ) programs/tests/econ_test.bin

# === libos32ai Module (AI意思決定エンジンライブラリ) ===
LIBAI_SRC = $(wildcard programs/libos32ai/*.c)
LIBAI_OBJ = $(LIBAI_SRC:.c=.o)

programs/libos32ai/%.o: programs/libos32ai/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === AI Test (libos32ai テスト) ===
programs/tests/ai_test.o: programs/tests/ai_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/ai_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/ai_test.o $(LIBAI_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/ai_test.o $(LIBAI_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ) -lc -lgcc

ai_test: $(CRT0_OBJ) programs/tests/ai_test.bin

# === libos32battle Module (ターンバトル解決エンジンライブラリ) ===
LIBBATTLE_SRC = $(wildcard programs/libos32battle/*.c)
LIBBATTLE_OBJ = $(LIBBATTLE_SRC:.c=.o)

programs/libos32battle/%.o: programs/libos32battle/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Battle Test (libos32battle テスト) ===
programs/tests/btl_test.o: programs/tests/btl_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/btl_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/btl_test.o $(LIBBATTLE_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/btl_test.o $(LIBBATTLE_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ) -lc -lgcc

btl_test: $(CRT0_OBJ) programs/tests/btl_test.bin

# === libos32board Module (ノードグラフ型ボードゲームエンジンライブラリ) ===
LIBBOARD_SRC = $(wildcard programs/libos32board/*.c)
LIBBOARD_OBJ = $(LIBBOARD_SRC:.c=.o)

programs/libos32board/%.o: programs/libos32board/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Board Test (libos32board テスト) ===
programs/tests/board_test.o: programs/tests/board_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/board_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/board_test.o $(LIBBOARD_OBJ) $(LIBOS32DB_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/board_test.o $(LIBBOARD_OBJ) $(LIBOS32DB_OBJ) -lc -lgcc

board_test: $(CRT0_OBJ) programs/tests/board_test.bin

# === libos32event Module (イベントスケジューラライブラリ) ===
LIBEVENT_SRC = $(wildcard programs/libos32event/*.c)
LIBEVENT_OBJ = $(LIBEVENT_SRC:.c=.o)

programs/libos32event/%.o: programs/libos32event/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Event Test (libos32event テスト) ===
programs/tests/evt_test.o: programs/tests/evt_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/evt_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/evt_test.o $(LIBEVENT_OBJ) $(LIBAI_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/evt_test.o $(LIBEVENT_OBJ) $(LIBAI_OBJ) $(LIBOS32DB_OBJ) $(LIBMATH_OBJ) -lc -lgcc

evt_test: $(CRT0_OBJ) programs/tests/evt_test.bin

# === libos32inv Module (インベントリ・装備・ショップエンジンライブラリ) ===
LIBINV_SRC = $(wildcard programs/libos32inv/*.c)
LIBINV_OBJ = $(LIBINV_SRC:.c=.o)

programs/libos32inv/%.o: programs/libos32inv/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

# === Inv Test (libos32inv テスト) ===
programs/tests/inv_test.o: programs/tests/inv_test.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

programs/tests/inv_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/inv_test.o $(LIBINV_OBJ) $(LIBOS32DB_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/inv_test.o $(LIBINV_OBJ) $(LIBOS32DB_OBJ) -lc -lgcc

inv_test: $(CRT0_OBJ) programs/tests/inv_test.bin

# === Gfx Demo Module ===
programs/libos32gfx/ui.o: programs/libos32gfx/ui.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/gfx_demo.o: programs/apps/gfx_demo.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/gfx_demo.elf: build/app.ld $(CRT0_OBJ) programs/apps/gfx_demo.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/gfx_demo.o $(GFX_OBJ) -lc -lgcc

gfx_demo: $(CRT0_OBJ) programs/apps/gfx_demo.bin

# === Demo1 Benchmark Module ===
programs/apps/demo1.o: programs/apps/demo1.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/demo1.elf: build/app.ld $(CRT0_OBJ) programs/apps/demo1.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/demo1.o $(GFX_OBJ) -lc -lgcc

demo1: $(CRT0_OBJ) programs/apps/demo1.bin

# === モジュール別コンパイルルール ===

# kernel/ モジュール
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS_BASE) $(INC_KERNEL) -c $< -o $@

# drivers/ モジュール
# mouse.c は idt.h (IRQ制御) を参照するため INC_KERNEL でビルド
drivers/mouse.o: drivers/mouse.c
	$(CC) $(CFLAGS_BASE) $(INC_KERNEL) -c $< -o $@

drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS_BASE) $(INC_DRIVERS) -c $< -o $@

# gfx/ モジュール
gfx/%.o: gfx/%.c
	$(CC) $(CFLAGS_BASE) $(INC_GFX) -c $< -o $@

# fs/ モジュール
fs/%.o: fs/%.c
	$(CC) $(CFLAGS_BASE) $(INC_FS) -c $< -o $@


# exec/ モジュール
exec/%.o: exec/%.c kapi/kapi_generated.c
	$(CC) $(CFLAGS_BASE) $(INC_EXEC) -c $< -o $@

# kapi/ モジュール
kapi/kapi_generated.c: tools/kapi.json tools/gen_kapi.py
	python3 tools/gen_kapi.py

kapi/%.o: kapi/%.c kapi/kapi_generated.c
	$(CC) $(CFLAGS_BASE) $(INC_KAPI) -c $< -o $@

# lib/ モジュール (汎用ライブラリ: カーネル依存なし)
lib/%.o: lib/%.c
	$(CC) $(CFLAGS_BASE) $(INC_LIB) -c $< -o $@

# SQLite (カーネル拡張域配置 — -Os必須)
lib/sqlite3/sqlite3.o: lib/sqlite3/sqlite3.c lib/sqlite3/os32_sqlite_config.h
	$(CC) $(CFLAGS_SQLITE) -include lib/sqlite3/os32_sqlite_config.h $(INC_SQLITE) -c $< -o $@

lib/sqlite3/os32_sqlite_vfs.o: lib/sqlite3/os32_sqlite_vfs.c lib/sqlite3/os32_sqlite_vfs.h lib/sqlite3/os32_sqlite_config.h
	$(CC) $(CFLAGS_SQLITE) -include lib/sqlite3/os32_sqlite_config.h $(INC_SQLITE) -c $< -o $@

lib/sqlite3/os32_sqlite_test.o: lib/sqlite3/os32_sqlite_test.c lib/sqlite3/os32_sqlite_vfs.h lib/sqlite3/os32_sqlite_config.h
	$(CC) -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O0 -fcommon -Wno-long-long -w -include lib/sqlite3/os32_sqlite_config.h $(INC_SQLITE) -c $< -o $@

# === Targets ===
all: boot kernel.bin sqlite.bin images/os32_boot.d88 programs iso

boot: $(BIN_STANDALONE)

%.bin: %.asm
	$(AS) -f bin $< -o $@

%.o: %.asm
	$(AS) -f elf32 $< -o $@

kernel.elf: $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) $(C_SQLITE_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ -lgcc

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary \
		--remove-section=.sqlite_text \
		--remove-section=.sqlite_rodata \
		--remove-section=.sqlite_data \
		--remove-section=.sqlite_bss \
		$< $@

# SQLite 拡張域バイナリ (0x18A000 にロードされる)
sqlite.bin: kernel.elf
	$(OBJCOPY) -O binary \
		--only-section=.sqlite_text \
		--only-section=.sqlite_rodata \
		--only-section=.sqlite_data \
		$< $@

# FDD最小ブートイメージ (images/os32_boot.d88)
# HDDインストール用ブートFD。必須コマンドのみ含む。
FDD_MIN_CMDS = more less grep find sort head tail wc tee touch hexdump sleep lzss diff du cal man sndctl
images/os32_boot.d88: boot kernel.bin programs lzss_dict unicode_bin
	@mkdir -p images
	@echo "=== Building OS32 minimal FDD image (images/os32_boot.d88) ==="
	@args="--tree"; \
	args="$$args /LOADER.BIN=boot/loader_fat.bin"; \
	args="$$args /kernel.bin=kernel.bin"; \
	args="$$args /sys/shell.bin=programs/shell.bin"; \
	args="$$args /sys/unicode.bin=unicode.bin"; \
	args="$$args /sys/boot_hdd.bin=boot/boot_hdd.bin"; \
	args="$$args /sys/loader_h.bin=boot/loader_hdd.bin"; \
	for cmd in $$(echo $(FDD_MIN_CMDS)); do \
		if [ -f "programs/cmds/$$cmd.bin" ]; then \
			args="$$args /bin/$$cmd.bin=programs/cmds/$$cmd.bin"; \
		elif [ -f "programs/system/$$cmd.bin" ]; then \
			args="$$args /bin/$$cmd.bin=programs/system/$$cmd.bin"; \
		fi; \
	done; \
	args="$$args /bin/edit.bin=programs/apps/edit.bin"; \
	args="$$args /sbin/install.bin=programs/system/install.bin"; \
	args="$$args /sbin/cdinst.bin=programs/system/cdinst.bin"; \
	if [ -f assets/profile_fdd ]; then args="$$args /etc/profile=assets/profile_fdd"; fi; \
	python3 tools/mkfat12.py -o images/os32_boot.img -b boot/boot_fat.bin -d images/os32_boot.d88 $$args
	@echo "Copying os32_boot.d88 to NP21/W directory..."
	@cp images/os32_boot.d88 '$(NP21W_DIR)/os32_boot.d88' 2>/dev/null || echo "Warning: Failed to copy os32_boot.d88 to np21w directory."

programs_base: $(CRT0_OBJ) $(BASE_PROGRAMS_BIN)

edit: $(CRT0_OBJ) programs/apps/edit.bin

lzss_dict: 
	@if [ ! -f tools/lzss_pack ]; then gcc tools/lzss_pack.c -O2 -o tools/lzss_pack; fi
	@if [ assets/SKK.DIC -nt assets/SKK.LZS ]; then tools/lzss_pack assets/SKK.DIC assets/SKK.LZS; fi

unicode_bin:
	@if [ ! -f tools/gen_unicode ]; then gcc tools/gen_unicode.c -I. -Iinclude -O2 -o tools/gen_unicode; fi
	@if [ ! -f unicode.bin ]; then ./tools/gen_unicode; fi

skk: $(CRT0_OBJ) programs/tests/skk_test.bin lzss_dict

programs/apps/spr_test.o: programs/apps/spr_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/spr_test.elf: build/app.ld $(CRT0_OBJ) programs/apps/spr_test.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/spr_test.o $(GFX_OBJ) -lc -lgcc

spr_test: $(CRT0_OBJ) programs/apps/spr_test.bin

# === VDP Viewer ===
programs/apps/vdpview.o: programs/apps/vdpview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/vdpview.elf: build/app.ld $(CRT0_OBJ) programs/apps/vdpview.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/vdpview.o $(GFX_OBJ) -lc -lgcc

vdpview: $(CRT0_OBJ) programs/apps/vdpview.bin



# === Raster Palette Demo ===
programs/apps/raster.o: programs/apps/raster.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/raster.elf: build/app.ld $(CRT0_OBJ) programs/apps/raster.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/raster.o $(GFX_OBJ) -lc -lgcc

raster: $(CRT0_OBJ) programs/apps/raster.bin

# === Ekakiuta (絵描き歌) ===
programs/apps/ekakiuta.o: programs/apps/ekakiuta.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/ekakiuta.elf: build/app.ld $(CRT0_OBJ) programs/apps/ekakiuta.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/ekakiuta.o $(GFX_OBJ) -lc -lgcc

ekakiuta: $(CRT0_OBJ) programs/apps/ekakiuta.bin

# === VBZ Vector Viewer ===
programs/apps/vbzview.o: programs/apps/vbzview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/vbzview.elf: build/app.ld $(CRT0_OBJ) programs/apps/vbzview.o $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(DBG_OBJ) programs/apps/vbzview.o $(GFX_OBJ) -lc -lgcc

vbzview: $(CRT0_OBJ) programs/apps/vbzview.bin

# === libos32snd (サウンドライブラリ) ===
programs/libos32snd/libos32snd.o: programs/libos32snd/libos32snd.c programs/libos32snd/libos32snd.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32snd -c $< -o $@

# === libmd (Markdownパーサー + レンダラーライブラリ) ===
programs/libmd/md_parse.o: programs/libmd/md_parse.c programs/libmd/libmd.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libmd -c $< -o $@

programs/libmd/md_render.o: programs/libmd/md_render.c programs/libmd/md_render.h programs/libmd/libmd.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libmd -Iprograms/libfiler -c $< -o $@

# === libfiler (GFXファイラーライブラリ + TVRAM描画) ===
programs/libfiler/filer_core.o: programs/libfiler/filer_core.c programs/libfiler/libfiler.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libfiler -c $< -o $@

programs/libfiler/filer_draw.o: programs/libfiler/filer_draw.c programs/libfiler/filer_draw.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libfiler -c $< -o $@

# === mdview (GFX Markdownビューア) ===
FILER_OBJ = programs/libfiler/filer_core.o
MDLIB_OBJ = programs/libmd/md_parse.o programs/libmd/md_render.o

programs/apps/mdview.o: programs/apps/mdview.c programs/libmd/libmd.h programs/libmd/md_render.h programs/libfiler/libfiler.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libmd -Iprograms/libfiler -c $< -o $@

programs/apps/mdview.elf: build/app.ld $(CRT0_OBJ) programs/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ) -lc -lgcc

mdview: $(CRT0_OBJ) programs/apps/mdview.bin

fep_dic:
	@if [ ! -f assets/fep.db ]; then python3 tools/fep_to_sqlite.py; fi

programs: $(DBG_OBJ) programs_base edit bench gfx_demo spr_test demo1 vdpview raster ekakiuta vbzview mdview lzss_cmd cdinst bench_scale2x pyxel_test gfx200_test gfx_demo200 blit_test blit_test2 demo_tile tile_bench rotate_test db_test e2test sqlite_standalone math_test chem_test chem_demo map_test map_demo input_test asset_test asset_demo ecs_test ecs_demo text_test text_demo econ_test ai_test btl_test board_test evt_test inv_test

# crt0.asm のアセンブル (外部プログラム用スタートアップ)
programs/crt0.o: programs/crt0.asm
	$(AS) -f elf32 $< -o $@

programs/libos32/help.o: programs/libos32/help.c programs/libos32/help.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/crt0_c.o: programs/crt0_c.c programs/libos32/help.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32/syscalls.o: programs/libos32/syscalls.c include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# $(DBG_OBJ) は上部の変数定義セクションで定義済み

programs/libos32/dbgserial.o: programs/libos32/dbgserial.c programs/libos32/dbgserial.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/cmds/%.elf: programs/cmds/%.c build/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o programs/cmds/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/cmds/$*.o -lc -lgcc

programs/apps/%.elf: programs/apps/%.c build/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o programs/apps/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/$*.o -lc -lgcc

programs/tests/%.elf: programs/tests/%.c build/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o programs/tests/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/$*.o -lc -lgcc

programs/tests/bench/%.elf: programs/tests/bench/%.c build/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o programs/tests/bench/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/bench/$*.o -lc -lgcc

programs/system/%.elf: programs/system/%.c build/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o programs/system/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/system/$*.o -lc -lgcc

programs/%.raw: programs/%.elf
	$(OBJCOPY) -O binary $< $@

programs/%.bin: programs/%.raw programs/%.elf
	@if [ "$*" = "install" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 8388608; \
	elif [ "$*" = "cdinst" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 8388608; \
	elif [ "$*" = "bench" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 262144; \
	elif [ "$*" = "gfx_demo" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "spr_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "demo1" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 1048576; \
	elif [ "$*" = "vdpview" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "skk_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 13 --heap 524288; \
	elif [ "$*" = "fep_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 524288; \
	elif [ "$*" = "edit" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 524288; \
	elif [ "$*" = "shell" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 1048576; \
	elif [ "$*" = "raster" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "ekakiuta" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "vbzview" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "mdview" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 1048576; \
	elif [ "$*" = "tests/bench_scale2x" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "tests/pyxel_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 27 --heap 2097152; \
	elif [ "$*" = "tests/gfx200_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 27 --heap 2097152; \
	elif [ "$*" = "tests/gfx_demo200" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 27 --heap 2097152; \
	elif [ "$*" = "tests/blit_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "tests/blit_test2" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "tests/demo_tile" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 28 --heap 524288; \
	elif [ "$*" = "tests/tile_bench" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 28 --heap 524288; \
	elif [ "$*" = "tests/rotate_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "tests/mouse_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 28; \
	elif [ "$*" = "tests/db_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29; \
	elif [ "$*" = "tests/chem_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/chem_demo" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/map_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 524288; \
	elif [ "$*" = "tests/map_demo" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 524288; \
	elif [ "$*" = "tests/input_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29; \
	elif [ "$*" = "tests/asset_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/asset_demo" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 524288; \
	elif [ "$*" = "tests/ecs_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29; \
	elif [ "$*" = "tests/ecs_demo" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/text_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/text_demo" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/econ_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/ai_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/btl_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/evt_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/inv_test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 29 --heap 262144; \
	elif [ "$*" = "tests/e2test" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 1048576; \
	elif [ "$*" = "tests/sqlite_standalone/sqlite_standalone" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 262144; \
	else \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7; \
	fi

# === デプロイ ===
HOSTDRV_DIR ?= /mnt/c/os32
NHD_DEPLOY = env NP21W_DIR='$(NP21W_DIR)' python3 tools/nhd_deploy.py
HOSTDRV_DEPLOY = env HOSTDRV_DIR='$(HOSTDRV_DIR)' python3 tools/hostdrv_deploy.py

# deploy: HostDrv方式 — ビルド成果物をC:\os32にコピー (sudo不要, 再起動不要)
# ゲストOSは /host 経由で直接アクセス可能
deploy: kernel.bin programs unicode_bin
	@echo "=== HostDrv Deploy ==="
	$(HOSTDRV_DEPLOY) sync

# deploy-kernel: カーネル+SQLiteをNHDブート領域に書き込み + HostDrvからext2同期 (NP21/W再起動が必要)
# C:\os32 (HostDrv) の内容をNHDのext2パーティションにも反映する
deploy-kernel: kernel.bin sqlite.bin
	$(NHD_DEPLOY) write-kernel kernel.bin boot/loader_hdd.bin sqlite.bin
	$(NHD_DEPLOY) sync-from-hostdrv
	$(NHD_DEPLOY) deploy

# deploy-nhd: 旧方式NHDフルデプロイ (カーネル+全ファイル)
deploy-nhd: kernel.bin programs unicode_bin
	@echo "=== NHD Deploy (using deploy.yaml) ==="
	$(NHD_DEPLOY) sync
	$(NHD_DEPLOY) deploy

# dp-<name>: 個別プログラムのビルド → シリアル経由でのホットデプロイ(再起動不要)
# NHDイメージへの書き込みを行わず、実行中のOS32へファイルを転送する
dp-%: programs/%.bin
	@echo "=== Hot Deploy (Serial Push): $*.bin ==="
	$(NHD_DEPLOY) push programs/$*.bin --resolve

# nhd-mount: NHDのext2パーティションをマウント
nhd-mount:
	$(NHD_DEPLOY) mount

# nhd-umount: NHDのext2パーティションをアンマウント
nhd-umount:
	$(NHD_DEPLOY) umount

# nhd-init: 初回セットアップ (Windows側NHDコピー + フォーマット + マウント)
nhd-init:
	$(NHD_DEPLOY) init

# === パッケージ / ISO生成 ===
packages: programs
	python3 tools/mkpkg.py --defs tools/package_defs.yaml --output packages/ --base .

iso: packages
	@mkdir -p images
	genisoimage -o images/os32_install.iso -V "OS32_INSTALL" -input-charset utf-8 -R packages/

clean:
	rm -f boot/*.bin $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) kernel.elf kernel.bin os.img os.d88 os_install.img os_install.d88 os_fat.img os_fat.d88 os_raw.img programs/cmds/*.o programs/cmds/*.elf programs/cmds/*.raw programs/cmds/*.bin programs/apps/*.o programs/apps/*.elf programs/apps/*.raw programs/apps/*.bin programs/tests/*.o programs/tests/*.elf programs/tests/*.raw programs/tests/*.bin programs/tests/bench/*.o programs/tests/bench/*.elf programs/tests/bench/*.raw programs/tests/bench/*.bin programs/tests/bench_scale2x/*.o programs/tests/bench_scale2x/*.elf programs/tests/bench_scale2x/*.raw programs/tests/bench_scale2x/*.bin programs/system/*.o programs/system/*.elf programs/system/*.raw programs/system/*.bin programs/crt0.o programs/shell/*.o programs/apps/edit/*.o programs/tests/bench/*.o programs/libos32math/*.o programs/libos32chem/*.o programs/libos32map/*.o programs/libos32input/*.o programs/libos32asset/*.o programs/libos32text/*.o programs/libos32econ/*.o programs/libos32ai/*.o programs/libos32battle/*.o programs/libos32inv/*.o programs/libos32gfx/*.o programs/libos32gfx/asm/*.o programs/libos32gfx/draw/*.o programs/libos32gfx/text/*.o programs/libos32gfx/geom/*.o programs/libpyxel/*.o programs/libtilemap/*.o programs/libos32/*.o programs/libmd/*.o programs/libfiler/*.o programs/libos32snd/*.o unicode.bin tools/gen_unicode
	rm -f lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o sqlite.bin
	rm -f kapi/kapi_db.o programs/libos32db/*.o programs/tests/db_test.o programs/tests/db_test.elf programs/tests/db_test.raw programs/tests/db_test.bin
	rm -f programs/tests/sqlite_standalone/*.o programs/tests/sqlite_standalone/*.elf programs/tests/sqlite_standalone/*.raw programs/tests/sqlite_standalone/*.bin
	rm -f packages/*.PKG images/os32_install.iso os32_boot.img os32_boot.d88
	rm -rf images

.PHONY: all boot build clean programs deploy deploy-kernel deploy-nhd nhd-mount nhd-umount nhd-init packages iso

# Add explicit dependencies for OS32X programs on the KAPI header
programs/%.o: include/os32_kapi_shared.h
$(shell find programs -name '*.o'): include/os32_kapi_shared.h
