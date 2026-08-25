# ============================================================================
#  OS32 標準アプリケーション集 — スタンドアロン Makefile
#
#  インストール済みの OS32 SDK だけを使ってビルドする。OS 側のソースツリーに
#  一切触れないので、このディレクトリを別リポジトリ (os32-apps) に切り出しても
#  そのまま動く。サードパーティアプリと同じ形。
#
#  使い方:
#    make OS32_SDK=/path/to/os32/build/sdk      全アプリ
#    make OS32_SDK=... edit                     個別
#    make OS32_SDK=... clean
#
#  アプリの追加: ディレクトリを掘って .c を置き、app.conf に 1 行足す。
#  既定以外の SDK ライブラリが要るなら下の LIBS_<名前> を書く。
# ============================================================================

OS32_SDK  ?= ../build/sdk
CROSS_DIR ?= /usr/local/cross

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
LD      = i386-elf-ld
OBJCOPY = i386-elf-objcopy

# アプリ = このディレクトリ直下のサブディレクトリ。1 アプリ 1 ディレクトリ。
APPS := $(notdir $(patsubst %/,%,$(wildcard */)))

CFLAGS = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector \
         -nostdlib -mno-red-zone -fcommon -O2 -Wall -MMD -MP -D__OS32_USERLAND__ \
         -I$(OS32_SDK)/include -I$(OS32_SDK)/include/os32 \
         -I$(CROSS_DIR)/i386-elf/include

LDFLAGS = -m elf_i386 -T $(OS32_SDK)/link/app.ld -nostdlib --nmagic --gc-sections \
          -L$(OS32_SDK)/lib -L$(CROSS_DIR)/i386-elf/lib \
          -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0

SDK_CRT = $(OS32_SDK)/crt/crt0.o $(OS32_SDK)/crt/crt0_c.o \
          $(OS32_SDK)/crt/syscalls.o $(OS32_SDK)/crt/help.o

# シリアルデバッグ出力を使うアプリ用。
SDK_DBG = $(OS32_SDK)/crt/dbgserial.o

# --- アプリごとのリンク指定 ---
# 既定は GFX (math は gfx が要求するので常に付ける)。
LIBS_DEFAULT = -los32gfx -los32math

LIBS_mdview  = -los32md -los32filer $(LIBS_DEFAULT)
LIBS_mgxview = -los32mgx -lzinflate -los32filer $(LIBS_DEFAULT)
LIBS_ui_demo = -los32ui $(LIBS_DEFAULT)

# dbgserial.o を追加するアプリ
EXTRA_mdview  = $(SDK_DBG)
EXTRA_vbzview = $(SDK_DBG)

libs_of  = $(if $(LIBS_$(1)),$(LIBS_$(1)),$(LIBS_DEFAULT))
extra_of = $(EXTRA_$(1))

# ---------------------------------------------------------------------------
#  DEFINE_APP — 1 アプリぶんのルール
#    $(1) = アプリ名 (= ディレクトリ名 = 出力バイナリ名)
# ---------------------------------------------------------------------------
define DEFINE_APP
$(1)_SRC := $$(wildcard $(1)/*.c)
$(1)_OBJ := $$($(1)_SRC:.c=.o)

$(1)/%.o: $(1)/%.c
	$$(CC) $$(CFLAGS) -I$(1) -c $$< -o $$@

$(1)/$(1).elf: $$($(1)_OBJ)
	$$(LD) $$(LDFLAGS) -o $$@ $$(SDK_CRT) $$(call extra_of,$(1)) $$($(1)_OBJ) \
	       --start-group $$(call libs_of,$(1)) --end-group -lc -lgcc

$(1): $(1)/$(1).bin
.PHONY: $(1)
endef

$(foreach a,$(APPS),$(eval $(call DEFINE_APP,$(a))))

%.raw: %.elf
	$(OBJCOPY) -O binary $< $@

# OS32X ヘッダを付ける。API 版とヒープは app.conf から読む。
# キーはこのディレクトリからの拡張子なしパス (例: edit/edit)。
%.bin: %.raw %.elf
	@_api=$$(awk '$$1 == "$*" { print $$2 }' app.conf); \
	_heap=$$(awk '$$1 == "$*" { print $$3 }' app.conf); \
	_api=$${_api:-$(KAPI_VERSION)}; _heap=$${_heap:-0}; \
	if [ "$$_heap" != "0" ]; then \
		python3 $(OS32_SDK)/bin/mkos32x.py $< $@ --elf $*.elf --api $$_api --heap $$_heap; \
	else \
		python3 $(OS32_SDK)/bin/mkos32x.py $< $@ --elf $*.elf --api $$_api; \
	fi

all: $(APPS)
	@echo "=== 標準アプリ $(words $(APPS)) 本 (SDK KAPI v$(KAPI_VERSION)) ==="

clean:
	rm -f */*.o */*.d */*.elf */*.raw */*.bin

.PHONY: all clean
-include $(wildcard */*.d)
