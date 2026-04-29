# ============================================================================
#  config.mk — ツールチェーン、フラグ、パス定義
# ============================================================================

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
INC_FS = $(INC_COMMON) -Ifs -Ifs/fatfs -Idrivers -Ikernel -Ilib

# FatFS: fs/fatfs/ ディレクトリ内のソースを独自の string.h スタブで解決
# -Ifs/fatfs を先頭に指定して <string.h> → fs/fatfs/string.h へ誘導
INC_FATFS = -Ifs/fatfs $(INC_COMMON) -Ifs -Idrivers -Ikernel -Ilib

# exec: 共通 + exec + kapi + fs + gfx + ドライバ (kbd依存)
INC_EXEC = $(INC_COMMON) -Iexec -Ikapi -Ifs -Igfx -Idrivers -Ilib -Ikernel

# KAPI: 全モジュール (全APIラッパーのため)
INC_KAPI = $(INC_COMMON) -Ikapi -Ikernel -Idrivers -Ifs -Iexec -Igfx -Ilib -Ilib/sqlite3

# lib: 共通 + 自身 (汎用ライブラリ: カーネル依存なし)
INC_LIB = $(INC_COMMON) -Ilib

# SQLite: カーネルFS/ドライバ + SQLiteヘッダ (VFS実装用)
INC_SQLITE = $(INC_COMMON) -Ilib/sqlite3 -Ifs -Idrivers -Ilib -Ikernel

# === コンパイルフラグ ===
CFLAGS_BASE = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O2 -Wall -fcommon -D__KERNEL_BUILD__
# SQLite専用フラグ: -Os (サイズ最適化, -O0のスタック肥大化回避) + -Wno-long-long (int64リテラル)
CFLAGS_SQLITE = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -Os -fcommon -ffunction-sections -fdata-sections -Wno-long-long -w -DNDEBUG -D__KERNEL_BUILD__
LDFLAGS = -m elf_i386 -T build/os32.ld -Map=kernel.map -nostdlib --nmagic --gc-sections \
	-L$(shell $(CC) -print-libgcc-file-name | xargs dirname)

# === 外部プログラム用フラグ ===
PROGRAM_FLAGS = $(CFLAGS_BASE) -I. -Iinclude -Iprograms -Iprograms/shell -Iprograms/libos32gfx -Iprograms/libos32math -Iprograms/libos32chem -Iprograms/libos32map -Iprograms/libos32input -Iprograms/libos32asset -Iprograms/libos32ecs -Iprograms/libos32text -Iprograms/libos32econ -Iprograms/libos32ai -Iprograms/libos32battle -Iprograms/libos32board -Iprograms/libos32event -Iprograms/libos32inv -Iprograms/libtilemap -I$(CROSS_DIR)/i386-elf/include
PROGRAM_LDFLAGS = -m elf_i386 -T build/app.ld -nostdlib --nmagic --gc-sections \
	-L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0

# === CRT0 / デバッグオブジェクト ===
CRT0_OBJ = programs/crt0.o programs/crt0_c.o programs/libos32/syscalls.o programs/libos32/help.o
DBG_OBJ  = programs/libos32/dbgserial.o

# === デプロイ先 ===
HOSTDRV_DIR ?= /mnt/c/os32
NHD_DEPLOY = env NP21W_DIR='$(NP21W_DIR)' python3 tools/nhd_deploy.py
HOSTDRV_DEPLOY = env HOSTDRV_DIR='$(HOSTDRV_DIR)' python3 tools/hostdrv_deploy.py
