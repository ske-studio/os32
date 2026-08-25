# ============================================================================
# programs.mk — 外部プログラム (OS32X) ビルドルール
# ============================================================================

# === ベースプログラム (単体ソースファイル → 自動ビルド) ===
C_CMDS = $(wildcard userland/cmds/*.c)
C_APPS = $(filter-out userland/apps/edit.c, $(wildcard userland/apps/*.c))
C_TESTS = $(filter-out userland/tests/gfx200_test.c userland/tests/gfx_demo200.c userland/tests/blit_test.c userland/tests/blit_test2.c userland/tests/demo_tile.c userland/tests/tile_bench.c userland/tests/rotate_test.c userland/tests/db_test.c userland/tests/dbq.c userland/tests/e2test.c userland/tests/math_test.c userland/tests/chem_test.c userland/tests/chem_demo.c userland/tests/map_test.c userland/tests/map_demo.c userland/tests/input_test.c userland/tests/asset_test.c userland/tests/asset_demo.c userland/tests/ecs_test.c userland/tests/ecs_demo.c userland/tests/text_test.c userland/tests/text_demo.c userland/tests/econ_test.c userland/tests/ai_test.c userland/tests/btl_test.c userland/tests/board_test.c userland/tests/evt_test.c userland/tests/inv_test.c userland/tests/turn_test.c userland/tests/rpg_test.c userland/tests/save_test.c userland/tests/mgx_test.c, $(wildcard userland/tests/*.c))
C_SYSTEM = $(filter-out userland/system/lz4.c userland/system/cdinst.c, $(wildcard userland/system/*.c))

C_BASE_PROGRAMS = $(C_CMDS) $(C_APPS) $(C_TESTS) $(C_SYSTEM)
BASE_PROGRAMS_BIN = $(C_BASE_PROGRAMS:.c=.bin) userland/shell.bin

# === CRT0 ビルドルール ===
sdk/crt/crt0.o: sdk/crt/crt0.asm
	$(AS) -f elf32 $< -o $@

sdk/crt/help.o: sdk/crt/help.c sdk/include/os32/help.h $(SDK_KAPI_HDR)
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

sdk/crt/crt0_c.o: sdk/crt/crt0_c.c sdk/include/os32/help.h $(SDK_KAPI_HDR)
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

sdk/crt/syscalls.o: sdk/crt/syscalls.c $(SDK_KAPI_HDR)
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

userland/lib/rt/dbgserial.o: userland/lib/rt/dbgserial.c userland/lib/rt/dbgserial.h $(SDK_KAPI_HDR)
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === Shell Module ===
SHELL_SRC = $(wildcard userland/shell/*.c)
SHELL_OBJ = $(SHELL_SRC:.c=.o)

userland/shell/%.o: userland/shell/%.c
	$(CC) $(PROGRAM_FLAGS) -Iuserland/shell $(INC_libos32filer) -c $< -o $@

userland/shell.elf: sdk/link/app_sys.ld $(CRT0_OBJ) $(SHELL_OBJ) $(FILER_DRAW_OBJ)
	$(LD) -m elf_i386 -T sdk/link/app_sys.ld -nostdlib --nmagic --gc-sections -L$(LIBDIR) -L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0 -o $@ $(CRT0_OBJ) $(SHELL_OBJ) $(LGRP_BEG) $(FILER_DRAW_OBJ) $(LGRP_END) -lc -lgcc

# === Edit (VZ-inspired Editor) Module ===
EDIT_SRC = $(wildcard userland/apps/edit/*.c)
EDIT_OBJ = $(EDIT_SRC:.c=.o)

userland/apps/edit/%.o: userland/apps/edit/%.c
	$(CC) $(PROGRAM_FLAGS) -Iuserland/apps/edit $(INC_libos32gfx) -Ilib -c $< -o $@

userland/apps/edit.elf: sdk/link/app.ld $(CRT0_OBJ) $(EDIT_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(EDIT_OBJ) $(LGRP_BEG) $(GFX_OBJ) $(LGRP_END) -lc -lgcc

# === Game (対戦スゴロクRPG) ===
# ゲームは game/Makefile が SDK 経由でビルドする。ここでは定義を持たない。
# 詳細は build/game.mk の game ターゲットを参照。

# (SKK Module / LZSS Command は廃止済み)

# === 共有 C ソースのユーザー空間ビルド ===
# lib/ のいくつかはカーネルと外部プログラムの双方から使う。カーネル側の
# lib/%.o は KERNEL_CFLAGS でビルドされる (build/kernel.mk) ため、
# そのオブジェクトを外部プログラムにリンクすると 1 つの .o が 2 つの
# リンクドメインに跨ることになる。ユーザー空間用は _prog.o として
# PROGRAM_FLAGS で別途ビルドする。
lib/utf8_prog.o: lib/utf8.c lib/utf8.h include/memmap.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

# === LZ4 Command ===
lib/lz4_prog.o: lib/lz4.c lib/lz4.h
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

userland/system/lz4.o: userland/system/lz4.c
	$(CC) $(PROGRAM_FLAGS) -Ilib -c $< -o $@

userland/system/lz4.elf: sdk/link/app.ld $(CRT0_OBJ) userland/system/lz4.o lib/lz4_prog.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/system/lz4.o lib/lz4_prog.o -lc -lgcc

lz4_cmd: $(CRT0_OBJ) userland/system/lz4.bin

# === CD Installer (cdinst) ===
userland/lib/rt/pkg.o: userland/lib/rt/pkg.c userland/lib/rt/pkg.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

userland/system/cdinst.o: userland/system/cdinst.c userland/lib/rt/pkg.h userland/lib/rt/dbgserial.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

userland/system/cdinst.elf: sdk/link/app.ld $(CRT0_OBJ) userland/system/cdinst.o userland/lib/rt/pkg.o $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/system/cdinst.o userland/lib/rt/pkg.o $(DBG_OBJ) -lc -lgcc

cdinst: $(CRT0_OBJ) userland/system/cdinst.bin

# === Bench Module ===
BENCH_SRC = $(wildcard userland/tests/bench/*.c)
BENCH_OBJ = $(BENCH_SRC:.c=.o)

userland/tests/bench/%.o: userland/tests/bench/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -c $< -o $@

userland/tests/bench.elf: sdk/link/app.ld $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_OBJ) $(LGRP_BEG) $(GFX_OBJ) $(LGRP_END) -lc -lgcc

bench: $(CRT0_OBJ) userland/tests/bench.bin

# === Bench Scale2x Module ===
BENCH_S2X_SRC = userland/tests/bench_scale2x/main.c
BENCH_S2X_OBJ = $(BENCH_S2X_SRC:.c=.o)

userland/tests/bench_scale2x/%.o: userland/tests/bench_scale2x/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -c $< -o $@

userland/tests/bench_scale2x.elf: sdk/link/app.ld $(CRT0_OBJ) $(BENCH_S2X_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_S2X_OBJ) $(LGRP_BEG) $(GFX_OBJ) $(LGRP_END) -lc -lgcc

bench_scale2x: $(CRT0_OBJ) userland/tests/bench_scale2x.bin

# ---------------------------------------------------------------------------
# DEFINE_TEST — テストプログラム定義テンプレート
# $(1) = テスト名 (tests/ 以下のベース名)
# $(2) = リンク対象 OBJ リスト
# $(3) = 追加コンパイルフラグ
# ---------------------------------------------------------------------------
# $(4) = テストの所在ディレクトリ (userland/tests または game/tests)
define DEFINE_TEST
$(4)/$(1).o: $(4)/$(1).c
	$$(CC) $$(PROGRAM_FLAGS) $(3) -c $$< -o $$@

$(4)/$(1).elf: sdk/link/app.ld $$(CRT0_OBJ) $(4)/$(1).o $(2)
	$$(LD) $$(PROGRAM_LDFLAGS) -o $$@ $$(CRT0_OBJ) $(4)/$(1).o $$(LGRP_BEG) $(2) $$(LGRP_END) -lc -lgcc

$(1): $$(CRT0_OBJ) $(4)/$(1).bin

.PHONY: $(1)
endef

# --- テストプログラム登録 ---

$(eval $(call DEFINE_TEST,gfx200_test,$$(GFX_OBJ),$$(INC_libos32gfx),userland/tests))
$(eval $(call DEFINE_TEST,gfx_demo200,$$(GFX_OBJ),$$(INC_libos32gfx),userland/tests))
$(eval $(call DEFINE_TEST,blit_test,$$(GFX_OBJ),$$(INC_libos32gfx),userland/tests))
$(eval $(call DEFINE_TEST,blit_test2,$$(GFX_OBJ),$$(INC_libos32gfx),userland/tests))
$(eval $(call DEFINE_TEST,rotate_test,$$(GFX_OBJ),$$(INC_libos32gfx),userland/tests))
$(eval $(call DEFINE_TEST,demo_tile,$$(TILEMAP_OBJ) $$(GFX_OBJ) $$(LIBASSET_OBJ),$$(INC_libos32tilemap),userland/tests))
$(eval $(call DEFINE_TEST,tile_bench,$$(TILEMAP_OBJ) $$(GFX_OBJ) $$(LIBASSET_OBJ),$$(INC_libos32tilemap),userland/tests))
$(eval $(call DEFINE_TEST,db_test,$$(LIBOS32DB_OBJ),$$(INC_libos32db),userland/tests))
$(eval $(call DEFINE_TEST,dbq,$$(LIBOS32DB_OBJ),$$(INC_libos32db),userland/tests))
$(eval $(call DEFINE_TEST,e2test,,,userland/tests))
$(eval $(call DEFINE_TEST,math_test,$$(LIBMATH_OBJ),$$(INC_libos32math),userland/tests))
$(eval $(call DEFINE_TEST,input_test,$$(LIBINPUT_OBJ) $$(LIBMATH_OBJ),$$(INC_libos32input),userland/tests))
$(eval $(call DEFINE_TEST,asset_test,$$(LIBASSET_OBJ),$$(INC_libos32asset),userland/tests))
$(eval $(call DEFINE_TEST,asset_demo,$$(LIBASSET_OBJ),$$(INC_libos32asset),userland/tests))
$(eval $(call DEFINE_TEST,ecs_test,$$(LIBECS_OBJ) $$(LIBMATH_OBJ),$$(INC_libos32ecs),userland/tests))
$(eval $(call DEFINE_TEST,save_test,$$(LIBSAVE_OBJ),$$(INC_libos32save),userland/tests))
$(eval $(call DEFINE_TEST,mgx_test,$$(LIBMGX_OBJ),$$(INC_libos32mgx),userland/tests))

# ---------------------------------------------------------------------------
# DEFINE_GFX_APP — GFXアプリ定義テンプレート
# $(1) = アプリのベース名 (apps/ 以下)
# $(2) = 追加 OBJ リスト (GFX_OBJ は常にリンク)
# $(3) = 追加コンパイルフラグ
# ---------------------------------------------------------------------------
define DEFINE_GFX_APP
userland/apps/$(1).o: userland/apps/$(1).c
	$$(CC) $$(PROGRAM_FLAGS) $$(INC_libos32gfx) $(3) -c $$< -o $$@

userland/apps/$(1).elf: sdk/link/app.ld $$(CRT0_OBJ) userland/apps/$(1).o $$(GFX_OBJ) $(2)
	$$(LD) $$(PROGRAM_LDFLAGS) -o $$@ $$(CRT0_OBJ) userland/apps/$(1).o $$(LGRP_BEG) $$(GFX_OBJ) $(2) $$(LGRP_END) -lc -lgcc

$(1): $$(CRT0_OBJ) userland/apps/$(1).bin

.PHONY: $(1)
endef

# --- GFXアプリ登録 ---
$(eval $(call DEFINE_GFX_APP,gfx_demo,,))
$(eval $(call DEFINE_GFX_APP,demo1,,))
$(eval $(call DEFINE_GFX_APP,spr_test,,))
$(eval $(call DEFINE_GFX_APP,vdpview,,))
$(eval $(call DEFINE_GFX_APP,raster,,))
$(eval $(call DEFINE_GFX_APP,ekakiuta,,))
$(eval $(call DEFINE_GFX_APP,mgxview,$$(LIBMGX_OBJ) $$(FILER_OBJ),$$(INC_libos32mgx) $$(INC_libos32filer)))

# vbzview は DBG_OBJ を追加
userland/apps/vbzview.o: userland/apps/vbzview.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -c $< -o $@

userland/apps/vbzview.elf: sdk/link/app.ld $(CRT0_OBJ) userland/apps/vbzview.o $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(DBG_OBJ) userland/apps/vbzview.o $(LGRP_BEG) $(GFX_OBJ) $(LGRP_END) -lc -lgcc

vbzview: $(CRT0_OBJ) userland/apps/vbzview.bin

# mdview
userland/apps/mdview.o: userland/apps/mdview.c userland/lib/md/libos32md.h userland/lib/md/md_render.h userland/lib/filer/libos32filer.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32md) $(INC_libos32filer) -c $< -o $@

userland/apps/mdview.elf: sdk/link/app.ld $(CRT0_OBJ) userland/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/apps/mdview.o $(DBG_OBJ) $(LGRP_BEG) $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(LGRP_END) -lc -lgcc

mdview: $(CRT0_OBJ) userland/apps/mdview.bin

# ui_demo — microUI デモ
userland/apps/ui_demo/ui_demo.o: userland/apps/ui_demo/ui_demo.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32ui) -c $< -o $@

userland/apps/ui_demo/ui_demo.elf: sdk/link/app.ld $(CRT0_OBJ) userland/apps/ui_demo/ui_demo.o $(GFX_OBJ) $(LIBUI_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/apps/ui_demo/ui_demo.o $(LGRP_BEG) $(GFX_OBJ) $(LIBUI_OBJ) $(LGRP_END) -lc -lgcc

ui_demo: $(CRT0_OBJ) userland/apps/ui_demo/ui_demo.bin

# libos32gfx/ui.o (gfx_demo が参照)
userland/lib/gfx/ui.o: userland/lib/gfx/ui.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -c $< -o $@

# === SQLite Standalone Test ===
SQLITE_SA_DIR = userland/tests/sqlite_standalone
SQLITE_SA_CFLAGS = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O1 -fcommon -Wno-long-long -w -I. -Iinclude $(SDK_INC) -Iuserland/lib -Ilib/sqlite3 -include lib/sqlite3/os32_sqlite_config.h -I$(CROSS_DIR)/i386-elf/include

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

$(SQLITE_SA_DIR)/sqlite_standalone.elf: sdk/link/app.ld $(SQLITE_SA_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(SQLITE_SA_OBJ) -lc -lgcc

sqlite_standalone: $(CRT0_OBJ) $(SQLITE_SA_DIR)/sqlite_standalone.bin

# === ベースプログラム パターンルール ===
userland/cmds/%.elf: userland/cmds/%.c sdk/link/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o userland/cmds/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/cmds/$*.o -lc -lgcc

userland/apps/%.elf: userland/apps/%.c sdk/link/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -c $< -o userland/apps/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/apps/$*.o -lc -lgcc

userland/tests/%.elf: userland/tests/%.c sdk/link/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o userland/tests/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/tests/$*.o -lc -lgcc

userland/tests/bench/%.elf: userland/tests/bench/%.c sdk/link/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o userland/tests/bench/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/tests/bench/$*.o -lc -lgcc

userland/system/%.elf: userland/system/%.c sdk/link/app.ld $(CRT0_OBJ)
	$(CC) $(PROGRAM_FLAGS) -c $< -o userland/system/$*.o
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) userland/system/$*.o -lc -lgcc

# === ELF → RAW → OS32X BIN 変換 ===
# ユーザーランドぶん。ゲームは game/Makefile が自前で持つ。
# build/app.conf のキーはリポジトリルートからの拡張子なしパス
# (例: userland/apps/edit)。キーが実在するターゲットと
# 一致しているかは make check-app-conf で検査できる。
userland/%.raw: userland/%.elf
	$(OBJCOPY) -O binary $< $@

userland/%.bin: userland/%.raw userland/%.elf
	@_api=$$(awk '$$1 == "userland/$*" { print $$2 }' build/app.conf); \
	_heap=$$(awk '$$1 == "userland/$*" { print $$3 }' build/app.conf); \
	_api=$${_api:-7}; \
	_heap=$${_heap:-0}; \
	if [ "$$_heap" != "0" ]; then \
		python3 sdk/mkos32x.py $< $@ --elf userland/$*.elf --api $$_api --heap $$_heap; \
	else \
		python3 sdk/mkos32x.py $< $@ --elf userland/$*.elf --api $$_api; \
	fi

# === ヘルパーツール ===
unicode_bin:
	@if [ ! -f tools/gen_unicode ]; then gcc tools/gen_unicode.c -I. -Iinclude -O2 -o tools/gen_unicode; fi
	@if [ ! -f $(BUILD_OUT)/unicode.bin ]; then ./tools/gen_unicode && mv unicode.bin $(BUILD_OUT)/unicode.bin; fi

fep_dic:
	@if [ ! -f assets/fep.db ]; then python3 tools/fep_to_sqlite.py; fi

# ============================================================================
# Rust プログラム ビルドルール
# ============================================================================
RUST_PROGRAMS_DIR = userland/rust
RUST_TARGET_JSON = i686-os32-none
RUST_TARGET_DIR = $(RUST_PROGRAMS_DIR)/target/$(RUST_TARGET_JSON)/release
RUST_KAPI_RS = sdk/rust/os32api/src/kapi_generated.rs

# Rustバインディング自動再生成 (kapi.json変更時)
$(RUST_KAPI_RS): sdk/kapi.json sdk/kapi_rust_gen.py
	python3 sdk/kapi_rust_gen.py

# ---------------------------------------------------------------------------
# DEFINE_RUST_PROGRAM — Rustプログラム定義テンプレート
# $(1) = Rustクレート名 (Cargoワークスペースメンバー名)
# $(2) = 出力先ディレクトリ (例: userland/tests)
# $(3) = 追加リンクOBJ (例: $(GFX_OBJ))
# ---------------------------------------------------------------------------
define DEFINE_RUST_PROGRAM
$(RUST_TARGET_DIR)/lib$(1).a: FORCE $(RUST_KAPI_RS)
	cd $(RUST_PROGRAMS_DIR) && cargo build --release -p $(1)

$(2)/$(1).elf: sdk/link/app.ld $$(CRT0_OBJ) $(RUST_TARGET_DIR)/lib$(1).a $(3)
	$$(LD) $$(PROGRAM_LDFLAGS) --allow-multiple-definition \
		-o $$@ $$(CRT0_OBJ) \
		$$(LGRP_BEG) $(3) $(RUST_TARGET_DIR)/lib$(1).a $$(LGRP_END) \
		-lc -lgcc

$(1)_rust: $$(CRT0_OBJ) $(2)/$(1).bin

.PHONY: $(1)_rust
endef

# --- Rustプログラム登録 ---
$(eval $(call DEFINE_RUST_PROGRAM,hello_gfx,userland/tests,$$(GFX_OBJ)))
$(eval $(call DEFINE_RUST_PROGRAM,alloc_demo,userland/tests,))
$(eval $(call DEFINE_RUST_PROGRAM,math_test_rs,userland/tests,))
$(eval $(call DEFINE_RUST_PROGRAM,font_test,userland/tests,$$(GFX_OBJ)))

# Rustクリーン
clean-rust:
	cd $(RUST_PROGRAMS_DIR) && cargo clean 2>/dev/null || true

FORCE:
.PHONY: clean-rust FORCE

# === プログラム集約ターゲット ===
# programs: に足し忘れたターゲットは make all でビルドされないまま
# tools/deploy.yaml が古いバイナリを NHD に残す (deploy.yaml 冒頭の警告を参照)。
# プログラムを追加したらこの一覧にも必ず足すこと。
programs_base: $(CRT0_OBJ) $(BASE_PROGRAMS_BIN)

edit: $(CRT0_OBJ) userland/apps/edit.bin

programs: $(DBG_OBJ) programs_base edit bench gfx_demo spr_test demo1 vdpview mgxview raster ekakiuta vbzview mdview ui_demo cdinst lz4_cmd bench_scale2x gfx200_test gfx_demo200 blit_test blit_test2 demo_tile tile_bench rotate_test db_test dbq e2test sqlite_standalone math_test input_test asset_test asset_demo ecs_test save_test mgx_test hello_gfx_rust alloc_demo_rust math_test_rs_rust font_test_rust

# === KAPI ヘッダ依存 ===
userland/%.o: $(SDK_KAPI_HDR)
$(shell find userland game -name '*.o' 2>/dev/null): $(SDK_KAPI_HDR)

# === プログラムクリーン ===
clean-programs: clean-rust
	rm -f userland/cmds/*.o userland/cmds/*.elf userland/cmds/*.raw userland/cmds/*.bin
	rm -f userland/apps/*.o userland/apps/*.elf userland/apps/*.raw userland/apps/*.bin
	rm -f userland/apps/edit/*.o
	rm -f userland/apps/ui_demo/*.o userland/apps/ui_demo/*.elf userland/apps/ui_demo/*.raw userland/apps/ui_demo/*.bin
	rm -f userland/tests/*.o userland/tests/*.elf userland/tests/*.raw userland/tests/*.bin
	rm -f userland/tests/bench/*.o userland/tests/bench/*.elf userland/tests/bench/*.raw userland/tests/bench/*.bin
	rm -f userland/tests/bench_scale2x/*.o userland/tests/bench_scale2x/*.elf userland/tests/bench_scale2x/*.raw userland/tests/bench_scale2x/*.bin
	rm -f userland/system/*.o userland/system/*.elf userland/system/*.raw userland/system/*.bin
	rm -f sdk/crt/*.o
	rm -f userland/shell/*.o

	rm -f userland/lib/rt/*.o
	rm -f userland/tests/sqlite_standalone/*.o userland/tests/sqlite_standalone/*.elf userland/tests/sqlite_standalone/*.raw userland/tests/sqlite_standalone/*.bin
	rm -f lib/lz4_prog.o lib/utf8_prog.o
	rm -f lib/zlib/*.o
	rm -f $(BUILD_OUT)/unicode.bin tools/gen_unicode

.PHONY: programs programs_base edit game lz4_cmd cdinst bench bench_scale2x
.PHONY: gfx200_test gfx_demo200 blit_test blit_test2 rotate_test
.PHONY: demo_tile tile_bench db_test e2test sqlite_standalone math_test
.PHONY: input_test
.PHONY: asset_test asset_demo ecs_test text_demo
.PHONY: inv_test
.PHONY: save_test
.PHONY: gfx_demo demo1 spr_test vdpview raster ekakiuta vbzview mdview
.PHONY: unicode_bin fep_dic
.PHONY: clean-programs
