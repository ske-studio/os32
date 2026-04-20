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
INC_KERNEL = $(INC_COMMON) -Ikernel -Idrivers -Ifs -Iexec -Igfx -Ilib -Ikapi

# ドライバ: 共通 + 自身 + gfx (kcg->gfx依存)
INC_DRIVERS = $(INC_COMMON) -Idrivers -Igfx -Ilib

# GFX: 共通 + 自身 + ドライバ (palette依存) + FS (dump時のファイル出力)
INC_GFX = $(INC_COMMON) -Igfx -Idrivers -Ifs

# FS: 共通 + 自身 + ドライバ (disk/ide依存)
INC_FS = $(INC_COMMON) -Ifs -Idrivers -Ikernel -Ilib


# exec: 共通 + exec + kapi + fs + gfx + ドライバ (kbd依存)
INC_EXEC = $(INC_COMMON) -Iexec -Ikapi -Ifs -Igfx -Idrivers -Ilib -Ikernel

# KAPI: 全モジュール (全APIラッパーのため)
INC_KAPI = $(INC_COMMON) -Ikapi -Ikernel -Idrivers -Ifs -Iexec -Igfx -Ilib

# lib: 共通 + 自身 (汎用ライブラリ: カーネル依存なし)
INC_LIB = $(INC_COMMON) -Ilib

# === コンパイルフラグ ===
CFLAGS_BASE = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O2 -Wall -fcommon
LDFLAGS = -m elf_i386 -T build/os32.ld -Map=kernel.map -nostdlib --nmagic --gc-sections

ASM_STANDALONE = boot/boot_fat.asm boot/loader_fat.asm boot/boot_hdd.asm boot/loader_hdd.asm
BIN_STANDALONE = $(ASM_STANDALONE:.asm=.bin)

ASM_KERNEL = kernel/kentry.asm kernel/isr_stub.asm kernel/setjmp.asm lib/kstring_asm.asm
ASM_KERNEL_OBJ = $(ASM_KERNEL:.asm=.o)

C_KERNEL = \
    kernel/kernel.c kernel/boot_splash.c kernel/idt.c kernel/isr_handlers.c \
    kernel/paging.c kernel/pgalloc.c kernel/shm.c kernel/kmalloc.c kernel/console.c kernel/sys.c \
    kernel/ime.c kernel/ime_romkana.c kernel/ime_dict.c kernel/snd_engine.c \
    drivers/kbd.c drivers/serial.c drivers/fm.c \
    drivers/fdc.c drivers/disk.c drivers/ide.c drivers/atapi.c drivers/rtc.c drivers/dev.c drivers/kcg.c drivers/np2sysp.c \
    gfx/gfx_core.c gfx/gfx_vram.c gfx/gfx_scroll.c gfx/palette.c \
    fs/fat12.c fs/ext2_super.c fs/ext2_inode.c fs/ext2_dir.c fs/ext2_file.c fs/ext2_fmt.c fs/ext2_vfs.c fs/vfs.c fs/vfs_fd.c fs/fd_redirect.c fs/pipe_buffer.c fs/serialfs.c fs/iso9660.c \
    exec/exec.c exec/exec_heap.c \
    kapi/kapi_generated.c \
    lib/path.c lib/utf8.c lib/kprintf.c lib/lzss.c lib/os_time.c lib/kstring.c

C_KERNEL_OBJ = $(C_KERNEL:.c=.o)

# Programs
PROGRAM_FLAGS = $(CFLAGS_BASE) -I. -Iinclude -Iprograms -Iprograms/shell -Iprograms/libos32gfx -I$(CROSS_DIR)/i386-elf/include
PROGRAM_LDFLAGS = -m elf_i386 -T build/app.ld -nostdlib --nmagic --gc-sections \
	-L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0

CRT0_OBJ = programs/crt0.o programs/crt0_c.o programs/libos32/syscalls.o programs/libos32/help.o
DBG_OBJ  = programs/libos32/dbgserial.o

C_CMDS    = $(wildcard programs/cmds/*.c)
C_APPS    = $(filter-out programs/apps/edit.c, $(wildcard programs/apps/*.c))
C_TESTS   = $(filter-out programs/tests/skk_test.c programs/tests/fep_test.c, $(wildcard programs/tests/*.c))
C_SYSTEM  = $(filter-out programs/system/lzss.c programs/system/cdinst.c, $(wildcard programs/system/*.c))

C_BASE_PROGRAMS = $(C_CMDS) $(C_APPS) $(C_TESTS) $(C_SYSTEM)
BASE_PROGRAMS_BIN = $(C_BASE_PROGRAMS:.c=.bin) programs/shell.bin

# === Shell Module ===
SHELL_SRC = $(wildcard programs/shell/*.c)
SHELL_OBJ = $(SHELL_SRC:.c=.o)

programs/shell/%.o: programs/shell/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/shell.elf: build/app_sys.ld $(CRT0_OBJ) $(SHELL_OBJ)
	$(LD) -m elf_i386 -T build/app_sys.ld -nostdlib --nmagic --gc-sections -L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0 -o $@ $(CRT0_OBJ) $(SHELL_OBJ) -lc -lgcc

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

# === FEP Test Module ===
lib/fep_engine_prog.o: lib/fep_engine.c lib/fep_engine.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/tests/fep_test.o: programs/tests/fep_test.c lib/fep_engine.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/tests/fep_test.elf: build/app.ld $(CRT0_OBJ) programs/tests/fep_test.o lib/fep_engine_prog.o lib/utf8.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/tests/fep_test.o lib/fep_engine_prog.o lib/utf8.o -lc -lgcc

fep_test: $(CRT0_OBJ) programs/tests/fep_test.bin

# === LZSS Command ===
# lib/lzss.c を外部プログラム用に再コンパイル
lib/lzss_prog.o: lib/lzss.c lib/lzss.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/system/lzss.o: programs/system/lzss.c
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/system/lzss.elf: build/app.ld $(CRT0_OBJ) programs/system/lzss.o lib/lzss_prog.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/system/lzss.o lib/lzss_prog.o -lc -lgcc

lzss_cmd: $(CRT0_OBJ) programs/system/lzss.bin

# === CD Installer (cdinst) ===
programs/libos32/pkg.o: programs/libos32/pkg.c programs/libos32/pkg.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/system/cdinst.o: programs/system/cdinst.c programs/libos32/pkg.h programs/libos32/dbgserial.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/system/cdinst.elf: build/app.ld $(CRT0_OBJ) programs/system/cdinst.o programs/libos32/pkg.o $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/system/cdinst.o programs/libos32/pkg.o $(DBG_OBJ) -lc -lgcc

cdinst: $(CRT0_OBJ) programs/system/cdinst.bin

# === OS32GFX Module ===
GFX_SRC = $(wildcard programs/libos32gfx/*.c) \
          $(wildcard programs/libos32gfx/draw/*.c) \
          $(wildcard programs/libos32gfx/text/*.c) \
          $(wildcard programs/libos32gfx/geom/*.c)
ASM_GFX_SRC = $(wildcard programs/libos32gfx/asm/*.asm)
ASM_GFX_OBJ = $(ASM_GFX_SRC:.asm=.o)
GFX_OBJ = $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ) lib/utf8.o

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

# === Targets ===
all: boot kernel.bin images/os32_boot.d88 programs iso

boot: $(BIN_STANDALONE)

%.bin: %.asm
	$(AS) -f bin $< -o $@

%.o: %.asm
	$(AS) -f elf32 $< -o $@

kernel.elf: $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

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

# === High-Res VDP Viewer ===
programs/apps/hrview.o: programs/apps/hrview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/hrview.elf: build/app.ld $(CRT0_OBJ) programs/apps/hrview.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/hrview.o $(GFX_OBJ) -lc -lgcc

hrview: $(CRT0_OBJ) programs/apps/hrview.bin

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

# === libmd (Markdownパーサーライブラリ) ===
programs/libmd/md_parse.o: programs/libmd/md_parse.c programs/libmd/libmd.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libmd -c $< -o $@

# === libfiler (GFXファイラーライブラリ) ===
programs/libfiler/filer_core.o: programs/libfiler/filer_core.c programs/libfiler/libfiler.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libfiler -c $< -o $@

# === mdview (GFX Markdownビューア) ===
FILER_OBJ = programs/libfiler/filer_core.o
MDLIB_OBJ = programs/libmd/md_parse.o

programs/apps/mdview.o: programs/apps/mdview.c programs/libmd/libmd.h programs/libfiler/libfiler.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libmd -Iprograms/libfiler -c $< -o $@

programs/apps/mdview.elf: build/app.ld $(CRT0_OBJ) programs/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ) -lc -lgcc

mdview: $(CRT0_OBJ) programs/apps/mdview.bin

fep_dic:
	@if [ ! -f assets/fep.dic ]; then python3 tools/fep_compiler.py -i assets/ipadic -o assets/fep.dic; fi

programs: $(DBG_OBJ) programs_base edit bench gfx_demo spr_test demo1 fep_test vdpview hrview raster ekakiuta vbzview mdview lzss_cmd cdinst

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
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 1048576; \
	elif [ "$*" = "hrview" ]; then \
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
	else \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7; \
	fi

# === NHD デプロイ ===
# NHDイメージのパス
NHD_DEPLOY = env NP21W_DIR='$(NP21W_DIR)' python3 tools/nhd_deploy.py

# deploy: カーネル+プログラム+データをNHDに書き込み → NP21/Wにコピー
# tools/deploy.yaml の定義に従って一括同期を行う
deploy: kernel.bin programs unicode_bin
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
	rm -f boot/*.bin $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) kernel.elf kernel.bin os.img os.d88 os_install.img os_install.d88 os_fat.img os_fat.d88 os_raw.img programs/cmds/*.o programs/cmds/*.elf programs/cmds/*.raw programs/cmds/*.bin programs/apps/*.o programs/apps/*.elf programs/apps/*.raw programs/apps/*.bin programs/tests/*.o programs/tests/*.elf programs/tests/*.raw programs/tests/*.bin programs/tests/bench/*.o programs/tests/bench/*.elf programs/tests/bench/*.raw programs/tests/bench/*.bin programs/system/*.o programs/system/*.elf programs/system/*.raw programs/system/*.bin programs/crt0.o programs/shell/*.o programs/apps/edit/*.o programs/tests/bench/*.o programs/libos32gfx/*.o programs/libos32gfx/asm/*.o programs/libos32gfx/draw/*.o programs/libos32gfx/text/*.o programs/libos32gfx/geom/*.o programs/libos32/*.o programs/libmd/*.o programs/libfiler/*.o programs/libos32snd/*.o unicode.bin tools/gen_unicode
	rm -f packages/*.PKG images/os32_install.iso os32_boot.img os32_boot.d88
	rm -rf images

.PHONY: all boot build clean programs deploy nhd-mount nhd-umount nhd-init packages iso

# Add explicit dependencies for OS32X programs on the KAPI header
programs/%.o: include/os32_kapi_shared.h
$(shell find programs -name '*.o'): include/os32_kapi_shared.h
