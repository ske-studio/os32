# ============================================================================
#  PC-9801 OS32 Makefile for GCC/NASM
#
#  リファクタリング版: build/*.mk による分割構成
#
#  主要ターゲット:
#    make all          全ビルド (カーネル + プログラム + イメージ)
#    make kernel       カーネル + SQLite のみ
#    make libs         全ユーザー空間ライブラリのみ
#    make programs     全外部プログラム
#    make boot         ブートローダーのみ
#    make deploy       HostDrvデプロイ
#    make deploy-kernel NHDブート領域書き込み
#    make clean        全クリーン
#
#  個別ターゲット:
#    make lib-<name>   個別ライブラリ (例: make lib-libos32math)
#    make <test_name>  個別テスト (例: make econ_test)
# ============================================================================

.DEFAULT_GOAL := all

# 環境変数を .env ファイルから読み込み (存在する場合)
-include .env

# === サブ Makefile のインクルード ===
include build/config.mk
include build/boot.mk
include build/kernel.mk
include build/libs.mk
include build/programs.mk
include build/deploy.mk
include build/image.mk
include build/sdk.mk

# === ヘッダ依存の取り込み ===
# 各 .c のコンパイル時に -MMD -MP (build/config.mk の DEPFLAGS) が生成する .d を読み込む。
# これにより構造体定義などヘッダを変更したとき、依存する .o が必ず再ビルドされる。
# Rust の target/ 以下にも .d があるため除外する。
DEPFILES := $(shell find boot kernel drivers gfx fs exec kapi lib programs sdk \
              -name '*.d' -not -path '*/target/*' 2>/dev/null)
-include $(DEPFILES)

# === 汎用パターンルール (ASM) ===
%.bin: %.asm
	$(AS) -f bin $< -o $@

%.o: %.asm
	$(AS) -f elf32 $< -o $@

# === 主要ターゲット ===
all: boot $(BUILD_OUT)/kernel.bin $(BUILD_OUT)/sqlite.bin $(BUILD_OUT)/vmkernel.lz4 images/os32_boot.d88 programs iso

# === クリーン (全サブモジュール) ===
clean: clean-kernel clean-programs clean-libs clean-images clean-sdk clean-deps
	rm -f os.img os.d88 os_install.img os_install.d88 os_raw.img

# 依存ファイル (.d) の一括削除 — Rust の target/ 以下は対象外
clean-deps:
	@find boot kernel drivers gfx fs exec kapi lib programs sdk \
	  -name '*.d' -not -path '*/target/*' -delete 2>/dev/null || true

.PHONY: all clean clean-deps
