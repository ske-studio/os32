# PC-9801 OS32 Makefile for GCC/NASM
#
# Phase 3 改修: モジュール別インクルードパス
# - カーネル共通: include/ のみ (types.h, io.h, console.h, tvram.h, os32_kapi_shared.h)
# - 各モジュール: 必要なサブディレクトリのみを -I で指定
# - 外部プログラム: include/ と programs/ のみ

.DEFAULT_GOAL := all

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
INC_FS = $(INC_COMMON) -Ifs -Idrivers


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
    kernel/ime.c kernel/ime_romkana.c kernel/ime_dict.c \
    drivers/kbd.c drivers/serial.c drivers/fm.c \
    drivers/fdc.c drivers/disk.c drivers/ide.c drivers/rtc.c drivers/dev.c drivers/kcg.c drivers/np2sysp.c \
    gfx/gfx_core.c gfx/gfx_vram.c gfx/gfx_scroll.c gfx/palette.c \
    fs/fat12.c fs/ext2_super.c fs/ext2_inode.c fs/ext2_dir.c fs/ext2_file.c fs/ext2_fmt.c fs/ext2_vfs.c fs/vfs.c fs/vfs_fd.c fs/fd_redirect.c fs/pipe_buffer.c fs/serialfs.c \
    exec/exec.c exec/exec_heap.c \
    kapi/kapi_generated.c \
    lib/path.c lib/utf8.c lib/kprintf.c lib/lzss.c lib/os_time.c lib/kstring.c

C_KERNEL_OBJ = $(C_KERNEL:.c=.o)

# Programs
PROGRAM_FLAGS = $(CFLAGS_BASE) -I. -Iinclude -Iprograms -Iprograms/shell -Iprograms/libos32gfx -I/home/hight/opt/cross/i386-elf/include
PROGRAM_LDFLAGS = -m elf_i386 -T build/app.ld -nostdlib --nmagic --gc-sections \
	-L/home/hight/opt/cross/i386-elf/lib -L/home/hight/opt/cross/lib/gcc/i386-elf/13.2.0

CRT0_OBJ = programs/crt0.o programs/crt0_c.o programs/libos32/syscalls.o programs/libos32/help.o

C_BASE_PROGRAMS = $(filter-out programs/skk_test.c programs/vz.c programs/crt0_c.c, $(wildcard programs/*.c))
BASE_PROGRAMS_BIN = $(C_BASE_PROGRAMS:.c=.bin) programs/shell.bin

# === Shell Module ===
SHELL_SRC = $(wildcard programs/shell/*.c)
SHELL_OBJ = $(SHELL_SRC:.c=.o)

programs/shell/%.o: programs/shell/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/shell.elf: build/app_sys.ld $(CRT0_OBJ) $(SHELL_OBJ)
	$(LD) -m elf_i386 -T build/app_sys.ld -nostdlib --nmagic --gc-sections -L/home/hight/opt/cross/i386-elf/lib -L/home/hight/opt/cross/lib/gcc/i386-elf/13.2.0 -o $@ $(CRT0_OBJ) $(SHELL_OBJ) -lc -lgcc

# === VZ Editor Module ===
VZ_SRC = $(wildcard programs/vz/*.c)
VZ_OBJ = $(VZ_SRC:.c=.o)

programs/vz/%.o: programs/vz/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/vz -c $< -o $@

programs/vz.elf: build/app.ld $(CRT0_OBJ) $(VZ_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(VZ_OBJ) $(GFX_OBJ) -lc -lgcc

# === SKK Module ===
SKK_SRC = $(wildcard programs/skk/*.c)
SKK_OBJ = $(SKK_SRC:.c=.o)

programs/skk/%.o: programs/skk/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/skk_test.o: programs/skk_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/skk_test.elf: build/app.ld $(CRT0_OBJ) programs/skk_test.o $(SKK_OBJ) lib/utf8.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/skk_test.o $(SKK_OBJ) lib/utf8.o -lc -lgcc

# === FEP Test Module ===
lib/fep_engine_prog.o: lib/fep_engine.c lib/fep_engine.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/fep_test.o: programs/fep_test.c lib/fep_engine.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

programs/fep_test.elf: build/app.ld $(CRT0_OBJ) programs/fep_test.o lib/fep_engine_prog.o lib/utf8.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/fep_test.o lib/fep_engine_prog.o lib/utf8.o -lc -lgcc

fep_test: $(CRT0_OBJ) programs/fep_test.bin

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
BENCH_SRC = $(wildcard programs/bench/*.c)
BENCH_OBJ = $(BENCH_SRC:.c=.o)

programs/bench/%.o: programs/bench/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/bench.elf: build/app.ld $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ) -lc -lgcc

bench: $(CRT0_OBJ) programs/bench.bin

# === Gfx Demo Module ===
programs/libos32gfx/ui.o: programs/libos32gfx/ui.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/gfx_demo.o: programs/gfx_demo.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/gfx_demo.elf: build/app.ld $(CRT0_OBJ) programs/gfx_demo.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/gfx_demo.o $(GFX_OBJ) -lc -lgcc

gfx_demo: $(CRT0_OBJ) programs/gfx_demo.bin

# === Demo1 Benchmark Module ===
programs/demo1.o: programs/demo1.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/demo1.elf: build/app.ld $(CRT0_OBJ) programs/demo1.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/demo1.o $(GFX_OBJ) -lc -lgcc

demo1: $(CRT0_OBJ) programs/demo1.bin

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
all: boot kernel.bin os.d88 programs

boot: $(BIN_STANDALONE)

%.bin: %.asm
	$(AS) -f bin $< -o $@

%.o: %.asm
	$(AS) -f elf32 $< -o $@

kernel.elf: $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

os.d88: boot kernel.bin programs lzss_dict unicode_bin
	@echo "Constructing OS32 image (os.img/os.d88)..."
	@args="LOADER.BIN=boot/loader_fat.bin KERNEL.BIN=kernel.bin BOOT_HDD.BIN=boot/boot_hdd.bin LOADER_H.BIN=boot/loader_hdd.bin TEST.TXT=programs/test.txt UTF8.TXT=programs/test_utf8.txt SKK.LZS=assets/SKK.LZS UNICODE.BIN=unicode.bin"; \
	for p in programs/*.bin; do \
		if [ -f "$$p" ]; then \
			name=`basename $$p | tr '[:lower:]' '[:upper:]'`; \
			args="$$args $$name=$$p"; \
		fi \
	done; \
	python3 tools/mkfat12.py -o os.img -b boot/boot_fat.bin -d os.d88 $$args
	@echo "Copying os.d88 to NP21/W directory..."
	@cp os.d88 '/mnt/c/Users/hight/OneDrive/ドキュメント/np21w/os.d88' 2>/dev/null || echo "Warning: Failed to copy os.d88 to np21w directory."

programs_base: $(CRT0_OBJ) $(BASE_PROGRAMS_BIN)

vz: $(CRT0_OBJ) programs/vz.bin

lzss_dict: 
	@if [ ! -f tools/lzss_pack ]; then gcc tools/lzss_pack.c -O2 -o tools/lzss_pack; fi
	@if [ assets/SKK.DIC -nt assets/SKK.LZS ]; then tools/lzss_pack assets/SKK.DIC assets/SKK.LZS; fi

unicode_bin:
	@if [ ! -f tools/gen_unicode ]; then gcc tools/gen_unicode.c -I. -Iinclude -O2 -o tools/gen_unicode; fi
	@if [ ! -f unicode.bin ]; then ./tools/gen_unicode; fi

skk: $(CRT0_OBJ) programs/skk_test.bin lzss_dict

programs/spr_test.o: programs/spr_test.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/spr_test.elf: build/app.ld $(CRT0_OBJ) programs/spr_test.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/spr_test.o $(GFX_OBJ) -lc -lgcc

spr_test: $(CRT0_OBJ) programs/spr_test.bin

# === VDP Viewer ===
programs/vdpview.o: programs/vdpview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/vdpview.elf: build/app.ld $(CRT0_OBJ) programs/vdpview.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/vdpview.o $(GFX_OBJ) -lc -lgcc

vdpview: $(CRT0_OBJ) programs/vdpview.bin

# === High-Res VDP Viewer ===
programs/hrview.o: programs/hrview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/hrview.elf: build/app.ld $(CRT0_OBJ) programs/hrview.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/hrview.o $(GFX_OBJ) -lc -lgcc

hrview: $(CRT0_OBJ) programs/hrview.bin

# === Raster Palette Demo ===
programs/raster.o: programs/raster.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/raster.elf: build/app.ld $(CRT0_OBJ) programs/raster.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/raster.o $(GFX_OBJ) -lc -lgcc

raster: $(CRT0_OBJ) programs/raster.bin

# === Ekakiuta (絵描き歌) ===
programs/ekakiuta.o: programs/ekakiuta.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/ekakiuta.elf: build/app.ld $(CRT0_OBJ) programs/ekakiuta.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/ekakiuta.o $(GFX_OBJ) -lc -lgcc

ekakiuta: $(CRT0_OBJ) programs/ekakiuta.bin

# === VBZ Vector Viewer ===
programs/vbzview.o: programs/vbzview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/vbzview.elf: build/app.ld $(CRT0_OBJ) programs/vbzview.o $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/vbzview.o $(GFX_OBJ) -lc -lgcc

vbzview: $(CRT0_OBJ) programs/vbzview.bin

fep_dic:
	@if [ ! -f assets/fep.dic ]; then python3 tools/fep_compiler.py -i assets/ipadic -o assets/fep.dic; fi

programs: programs_base vz skk bench gfx_demo spr_test demo1 fep_test vdpview hrview raster ekakiuta vbzview

# crt0.asm のアセンブル (外部プログラム用スタートアップ)
programs/crt0.o: programs/crt0.asm
	$(AS) -f elf32 $< -o $@

programs/libos32/help.o: programs/libos32/help.c programs/libos32/help.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/crt0_c.o: programs/crt0_c.c programs/libos32/help.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32/syscalls.o: programs/libos32/syscalls.c include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/%.elf: programs/%.c build/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o programs/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/$*.o -lc -lgcc

programs/%.raw: programs/%.elf
	$(OBJCOPY) -O binary $< $@

programs/%.bin: programs/%.raw programs/%.elf
	@if [ "$*" = "install" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 262144; \
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
	elif [ "$*" = "vz" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 524288; \
	elif [ "$*" = "shell" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7 --heap 1048576; \
	elif [ "$*" = "raster" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "ekakiuta" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	elif [ "$*" = "vbzview" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
	else \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 7; \
	fi

# === NHD デプロイ ===
# NHDイメージのパス
NHD_DEPLOY = python3 tools/nhd_deploy.py

# ファイル分類
SBIN_PROGRAMS = programs/install.bin programs/mem.bin programs/crash.bin programs/bench.bin
USR_BIN_PROGRAMS = programs/vz.bin programs/gfx_demo.bin programs/demo1.bin \
	programs/ekakiuta.bin programs/vbzview.bin programs/hrview.bin programs/vdpview.bin \
	programs/spr_test.bin programs/raster.bin programs/skk_test.bin programs/fep_test.bin
# /bin: 上記以外の全 .bin (shell除く)
BIN_PROGRAMS = $(filter-out programs/shell.bin $(SBIN_PROGRAMS) $(USR_BIN_PROGRAMS), $(wildcard programs/*.bin))

# deploy: カーネル+プログラム+データをNHDに書き込み → NP21/Wにコピー
deploy: kernel.bin programs unicode_bin
	@echo "=== NHD Deploy ==="
	$(NHD_DEPLOY) setup-dirs
	$(NHD_DEPLOY) write-kernel kernel.bin boot/loader_hdd.bin
	$(NHD_DEPLOY) copy --dest / --rename shell programs/shell.bin
	$(NHD_DEPLOY) copy --dest /bin $(BIN_PROGRAMS)
	$(NHD_DEPLOY) copy --dest /sbin $(SBIN_PROGRAMS)
	$(NHD_DEPLOY) copy --dest /usr/bin $(USR_BIN_PROGRAMS)
	$(NHD_DEPLOY) copy --dest /data unicode.bin
	@if [ -d docs/manpages ] && ls docs/manpages/*.1 1>/dev/null 2>&1; then \
		$(NHD_DEPLOY) copy --dest /usr/man $(shell ls docs/manpages/*.1 2>/dev/null); \
	fi
	$(NHD_DEPLOY) deploy

# dp-<name>: 個別プログラムのビルド → NHDコピー → NP21/Wデプロイ
# 使い方: make dp-ekakiuta, make dp-shell, make dp-vz 等
# ※ カーネル変更時は make deploy (全体) を使うこと
# ファイルの配置先を自動判定
dp-%: programs/%.bin
	@echo "=== Deploy: $*.bin ==="
	@if [ "$*" = "shell" ]; then \
		$(NHD_DEPLOY) copy --dest / --rename shell programs/$*.bin; \
	elif echo "$(SBIN_PROGRAMS)" | grep -q "programs/$*.bin"; then \
		$(NHD_DEPLOY) copy --dest /sbin programs/$*.bin; \
	elif echo "$(USR_BIN_PROGRAMS)" | grep -q "programs/$*.bin"; then \
		$(NHD_DEPLOY) copy --dest /usr/bin programs/$*.bin; \
	else \
		$(NHD_DEPLOY) copy --dest /bin programs/$*.bin; \
	fi
	$(NHD_DEPLOY) deploy

# nhd-mount: NHDのext2パーティションをマウント
nhd-mount:
	$(NHD_DEPLOY) mount

# nhd-umount: NHDのext2パーティションをアンマウント
nhd-umount:
	$(NHD_DEPLOY) umount

# nhd-init: 初回セットアップ (Windows側NHDコピー + フォーマット + マウント)
nhd-init:
	$(NHD_DEPLOY) init

clean:
	rm -f boot/*.bin $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) kernel.elf kernel.bin os.img os.d88 os_install.img os_install.d88 os_fat.img os_fat.d88 os_raw.img programs/*.o programs/*.elf programs/*.raw programs/*.bin programs/crt0.o programs/shell/*.o programs/vz/*.o programs/bench/*.o programs/libos32gfx/*.o programs/libos32gfx/asm/*.o programs/libos32gfx/draw/*.o programs/libos32gfx/text/*.o programs/libos32gfx/geom/*.o programs/libos32/*.o unicode.bin tools/gen_unicode

.PHONY: all boot build clean programs deploy nhd-mount nhd-umount nhd-init

# Add explicit dependencies for OS32X programs on the KAPI header
programs/%.o: include/os32_kapi_shared.h
$(shell find programs -name '*.o'): include/os32_kapi_shared.h
