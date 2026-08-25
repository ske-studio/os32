# ============================================================================
#  OS32 対戦スゴロクRPG — スタンドアロン Makefile
#
#  インストール済みの OS32 SDK だけを使ってビルドする。OS 側のソースツリーに
#  一切触れないので、このディレクトリを別リポジトリ (os32-game) に切り出しても
#  そのまま動く。
#
#  使い方:
#    make OS32_SDK=/path/to/os32/build/sdk          ゲーム本体 + テスト
#    make OS32_SDK=... data                         マスタDBを生成
#    make OS32_SDK=... clean
#
#  モノレポ内からは build/programs.mk が
#    $(MAKE) -C game OS32_SDK=$(abspath build/sdk)
#  として呼ぶ。
# ============================================================================

OS32_SDK ?= ../build/sdk
CROSS_DIR ?= /usr/local/cross

# SDK の存在チェックはビルド系のターゲットでだけ行う。clean は SDK が
# 無くても通らなければならない (OS 側の make clean は clean-sdk で SDK を
# 消してから clean-game を呼ぶため)。
SDK_STAMP := $(wildcard $(OS32_SDK)/KAPI_VERSION)
ifeq ($(SDK_STAMP),)
  ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),all)),)
    $(error OS32_SDK=$(OS32_SDK) に SDK が見つからない。OS 側で make sdk を実行すること)
  endif
  KAPI_VERSION = 0
else
  KAPI_VERSION := $(shell cat $(OS32_SDK)/KAPI_VERSION)
endif

CC      = i386-elf-gcc
AS      = nasm
LD      = i386-elf-ld
OBJCOPY = i386-elf-objcopy

# --- ゲーム自身のライブラリ ---
GAME_LIB_DIRS = ai battle board chem econ event inv map rpg text turn
GAME_LIB_INC  = $(foreach d,$(GAME_LIB_DIRS),-Ilib/$(d))

# --- SDK ---
#   include/       プラットフォームライブラリの公開ヘッダ (libos32gfx.h 等)
#   include/os32/  KAPI 契約ヘッダ (os32api.h, os32_kapi_shared.h)
#   include/rt/    ランタイム小物 (dbgserial.h, pkg.h)
SDK_INC = -I$(OS32_SDK)/include -I$(OS32_SDK)/include/os32
SDK_CRT = $(OS32_SDK)/crt/crt0.o $(OS32_SDK)/crt/crt0_c.o \
          $(OS32_SDK)/crt/syscalls.o $(OS32_SDK)/crt/help.o

CFLAGS = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector \
         -nostdlib -mno-red-zone -fcommon -O2 -Wall -MMD -MP -D__OS32_USERLAND__ \
         $(SDK_INC) $(GAME_LIB_INC) -Iapp -I$(CROSS_DIR)/i386-elf/include

LDFLAGS = -m elf_i386 -T $(OS32_SDK)/link/app.ld -nostdlib --nmagic --gc-sections \
          -L$(OS32_SDK)/lib -L$(CROSS_DIR)/i386-elf/lib \
          -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0

# 使うプラットフォームライブラリ。--start-group で囲むので並び順は問わない。
SDK_LIBS = -los32gfx -los32math -los32tilemap -los32asset -los32db -los32snd \
           -los32save -los32ecs

# --- ソース ---
APP_SRC  = $(wildcard app/*.c)
LIB_SRC  = $(foreach d,$(GAME_LIB_DIRS),$(wildcard lib/$(d)/*.c))
APP_OBJ  = $(APP_SRC:.c=.o)
LIB_OBJ  = $(LIB_SRC:.c=.o)

# --- テスト (1 ソース = 1 プログラム) ---
TEST_SRC = $(wildcard tests/*.c)
TEST_BIN = $(TEST_SRC:.c=.bin)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

app/game.elf: $(SDK_CRT) $(APP_OBJ) $(LIB_OBJ)
	$(LD) $(LDFLAGS) -o $@ $(SDK_CRT) $(APP_OBJ) $(LIB_OBJ) \
	      --start-group $(SDK_LIBS) --end-group -lc -lgcc

tests/%.elf: tests/%.o $(LIB_OBJ)
	$(LD) $(LDFLAGS) -o $@ $(SDK_CRT) $< $(LIB_OBJ) \
	      --start-group $(SDK_LIBS) --end-group -lc -lgcc

%.raw: %.elf
	$(OBJCOPY) -O binary $< $@

# OS32X ヘッダを付ける。API 版とヒープは app.conf から読む。
%.bin: %.raw %.elf
	@_api=$$(awk '$$1 == "$*" { print $$2 }' app.conf); \
	_heap=$$(awk '$$1 == "$*" { print $$3 }' app.conf); \
	_api=$${_api:-$(KAPI_VERSION)}; _heap=$${_heap:-0}; \
	if [ "$$_heap" != "0" ]; then \
		python3 $(OS32_SDK)/bin/mkos32x.py $< $@ --elf $*.elf --api $$_api --heap $$_heap; \
	else \
		python3 $(OS32_SDK)/bin/mkos32x.py $< $@ --elf $*.elf --api $$_api; \
	fi

all: app/game.bin $(TEST_BIN)
	@echo "=== ゲーム本体 + テスト $(words $(TEST_BIN)) 本 (SDK KAPI v$(KAPI_VERSION)) ==="

# --- マスタデータ ---
DB_DIR = build/db
include data/data.mk

clean:
	rm -f app/*.o app/*.d app/*.elf app/*.raw app/*.bin
	rm -f lib/*/*.o lib/*/*.d
	rm -f tests/*.o tests/*.d tests/*.elf tests/*.raw tests/*.bin
	rm -rf build

.PHONY: all clean data
-include $(shell find app lib tests -name '*.d' 2>/dev/null)
