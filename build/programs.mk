# ============================================================================
#  programs.mk — 外部プログラム (OS32X) ビルドルール
# ============================================================================

# === ベースプログラム (単体ソースファイル → 自動ビルド) ===
C_CMDS    = $(wildcard programs/cmds/*.c)
C_APPS    = $(filter-out programs/apps/edit.c, $(wildcard programs/apps/*.c))
C_TESTS   = $(filter-out programs/tests/fep_test.c programs/tests/gfx200_test.c programs/tests/gfx_demo200.c programs/tests/blit_test.c programs/tests/blit_test2.c programs/tests/demo_tile.c programs/tests/tile_bench.c programs/tests/rotate_test.c programs/tests/db_test.c programs/tests/e2test.c programs/tests/math_test.c programs/tests/chem_test.c programs/tests/chem_demo.c programs/tests/map_test.c programs/tests/map_demo.c programs/tests/input_test.c programs/tests/asset_test.c programs/tests/asset_demo.c programs/tests/ecs_test.c programs/tests/ecs_demo.c programs/tests/text_test.c programs/tests/text_demo.c programs/tests/econ_test.c programs/tests/ai_test.c programs/tests/btl_test.c programs/tests/board_test.c programs/tests/evt_test.c programs/tests/inv_test.c, $(wildcard programs/tests/*.c))
C_SYSTEM  = $(filter-out programs/system/lz4.c programs/system/cdinst.c, $(wildcard programs/system/*.c))

C_BASE_PROGRAMS = $(C_CMDS) $(C_APPS) $(C_TESTS) $(C_SYSTEM)
BASE_PROGRAMS_BIN = $(C_BASE_PROGRAMS:.c=.bin) programs/shell.bin

# === CRT0 ビルドルール ===
programs/crt0.o: programs/crt0.asm
	$(AS) -f elf32 $< -o $@

programs/libos32/help.o: programs/libos32/help.c programs/libos32/help.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/crt0_c.o: programs/crt0_c.c programs/libos32/help.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32/syscalls.o: programs/libos32/syscalls.c include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32/dbgserial.o: programs/libos32/dbgserial.c programs/libos32/dbgserial.h include/os32_kapi_shared.h
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === Shell Module ===
SHELL_SRC = $(wildcard programs/shell/*.c)
SHELL_OBJ = $(SHELL_SRC:.c=.o)

programs/shell/%.o: programs/shell/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32filer -c $< -o $@

programs/shell.elf: build/app_sys.ld $(CRT0_OBJ) $(SHELL_OBJ) $(FILER_DRAW_OBJ)
	$(LD) -m elf_i386 -T build/app_sys.ld -nostdlib --nmagic --gc-sections -L$(CROSS_DIR)/i386-elf/lib -L$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0 -o $@ $(CRT0_OBJ) $(SHELL_OBJ) $(FILER_DRAW_OBJ) -lc -lgcc

# === Edit (VZ-inspired Editor) Module ===
EDIT_SRC = $(wildcard programs/apps/edit/*.c)
EDIT_OBJ = $(EDIT_SRC:.c=.o)

programs/apps/edit/%.o: programs/apps/edit/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/apps/edit -c $< -o $@

programs/apps/edit.elf: build/app.ld $(CRT0_OBJ) $(EDIT_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(EDIT_OBJ) $(GFX_OBJ) -lc -lgcc

# === Game (対戦スゴロクRPG) Module ===
GAME_SRC = $(wildcard programs/apps/game/*.c)
GAME_OBJ = $(GAME_SRC:.c=.o)
GAME_LIBS = $(GFX_OBJ) $(LIBUI_OBJ) $(LIBBOARD_OBJ) $(LIBBATTLE_OBJ) \
            $(LIBECON_OBJ) $(LIBINV_OBJ) $(LIBAI_OBJ) $(LIBOS32DB_OBJ)

programs/apps/game/%.o: programs/apps/game/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/apps/game -Iprograms/libos32db -c $< -o $@

programs/apps/game.elf: build/app.ld $(CRT0_OBJ) $(GAME_OBJ) $(GAME_LIBS)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(GAME_OBJ) $(GAME_LIBS) -lc -lgcc

# (SKK Module / LZSS Command は廃止済み)

# === LZ4 Command ===
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

# === Bench Module ===
BENCH_SRC = $(wildcard programs/tests/bench/*.c)
BENCH_OBJ = $(BENCH_SRC:.c=.o)

programs/tests/bench/%.o: programs/tests/bench/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/bench.elf: build/app.ld $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_OBJ) $(GFX_OBJ) -lc -lgcc

bench: $(CRT0_OBJ) programs/tests/bench.bin

# === Bench Scale2x Module ===
BENCH_S2X_SRC = programs/tests/bench_scale2x/main.c
BENCH_S2X_OBJ = $(BENCH_S2X_SRC:.c=.o)

programs/tests/bench_scale2x/%.o: programs/tests/bench_scale2x/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/tests/bench_scale2x.elf: build/app.ld $(CRT0_OBJ) $(BENCH_S2X_OBJ) $(GFX_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(BENCH_S2X_OBJ) $(GFX_OBJ) -lc -lgcc

bench_scale2x: $(CRT0_OBJ) programs/tests/bench_scale2x.bin

# ---------------------------------------------------------------------------
#  DEFINE_TEST — テストプログラム定義テンプレート
#  $(1) = テスト名 (tests/ 以下のベース名)
#  $(2) = リンク対象 OBJ リスト
#  $(3) = 追加コンパイルフラグ
# ---------------------------------------------------------------------------
define DEFINE_TEST
programs/tests/$(1).o: programs/tests/$(1).c
	$$(CC) $$(PROGRAM_FLAGS) $(3) -c $$< -o $$@

programs/tests/$(1).elf: build/app.ld $$(CRT0_OBJ) programs/tests/$(1).o $(2)
	$$(LD) $$(PROGRAM_LDFLAGS) -o $$@ $$(CRT0_OBJ) programs/tests/$(1).o $(2) -lc -lgcc

$(1): $$(CRT0_OBJ) programs/tests/$(1).bin

.PHONY: $(1)
endef

# --- テストプログラム登録 ---

$(eval $(call DEFINE_TEST,gfx200_test,$$(GFX_OBJ),))
$(eval $(call DEFINE_TEST,gfx_demo200,$$(GFX_OBJ),))
$(eval $(call DEFINE_TEST,blit_test,$$(GFX_OBJ),))
$(eval $(call DEFINE_TEST,blit_test2,$$(GFX_OBJ),))
$(eval $(call DEFINE_TEST,rotate_test,$$(GFX_OBJ),))
$(eval $(call DEFINE_TEST,demo_tile,$$(TILEMAP_OBJ) $$(GFX_OBJ) $$(LIBASSET_OBJ),))
$(eval $(call DEFINE_TEST,tile_bench,$$(TILEMAP_OBJ) $$(GFX_OBJ) $$(LIBASSET_OBJ),))
$(eval $(call DEFINE_TEST,db_test,$$(LIBOS32DB_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,e2test,,))
$(eval $(call DEFINE_TEST,math_test,$$(LIBMATH_OBJ),))
$(eval $(call DEFINE_TEST,chem_test,$$(LIBCHEM_OBJ) $$(LIBOS32DB_OBJ) $$(LIBMATH_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,chem_demo,$$(LIBCHEM_OBJ) $$(LIBOS32DB_OBJ) $$(GFX_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,map_test,$$(LIBMAP_OBJ) $$(LIBOS32DB_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,map_demo,$$(LIBMAP_OBJ) $$(LIBOS32DB_OBJ) $$(TILEMAP_OBJ) $$(GFX_OBJ) $$(LIBASSET_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,input_test,$$(LIBINPUT_OBJ) $$(LIBMATH_OBJ),))
$(eval $(call DEFINE_TEST,asset_test,$$(LIBASSET_OBJ),))
$(eval $(call DEFINE_TEST,asset_demo,$$(LIBASSET_OBJ),))
$(eval $(call DEFINE_TEST,ecs_test,$$(LIBECS_OBJ) $$(LIBMATH_OBJ),))
$(eval $(call DEFINE_TEST,ecs_demo,$$(LIBECS_OBJ) $$(LIBCHEM_OBJ) $$(LIBOS32DB_OBJ) $$(LIBMATH_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,text_test,$$(LIBTEXT_OBJ) $$(LIBOS32DB_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,text_demo,$$(LIBTEXT_OBJ) $$(LIBOS32DB_OBJ) $$(GFX_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,econ_test,$$(LIBECON_OBJ) $$(LIBOS32DB_OBJ) $$(LIBMATH_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,ai_test,$$(LIBAI_OBJ) $$(LIBOS32DB_OBJ) $$(LIBMATH_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,btl_test,$$(LIBBATTLE_OBJ) $$(LIBOS32DB_OBJ) $$(LIBMATH_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,board_test,$$(LIBBOARD_OBJ) $$(LIBOS32DB_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,evt_test,$$(LIBEVENT_OBJ) $$(LIBAI_OBJ) $$(LIBOS32DB_OBJ) $$(LIBMATH_OBJ),-Iprograms/libos32db))
$(eval $(call DEFINE_TEST,inv_test,$$(LIBINV_OBJ) $$(LIBOS32DB_OBJ),-Iprograms/libos32db))

# ---------------------------------------------------------------------------
#  DEFINE_GFX_APP — GFXアプリ定義テンプレート
#  $(1) = アプリのベース名 (apps/ 以下)
#  $(2) = 追加 OBJ リスト (GFX_OBJ は常にリンク)
#  $(3) = 追加コンパイルフラグ
# ---------------------------------------------------------------------------
define DEFINE_GFX_APP
programs/apps/$(1).o: programs/apps/$(1).c
	$$(CC) $$(PROGRAM_FLAGS) $(3) -c $$< -o $$@

programs/apps/$(1).elf: build/app.ld $$(CRT0_OBJ) programs/apps/$(1).o $$(GFX_OBJ) $(2)
	$$(LD) $$(PROGRAM_LDFLAGS) -o $$@ $$(CRT0_OBJ) programs/apps/$(1).o $$(GFX_OBJ) $(2) -lc -lgcc

$(1): $$(CRT0_OBJ) programs/apps/$(1).bin

.PHONY: $(1)
endef

# --- GFXアプリ登録 ---
$(eval $(call DEFINE_GFX_APP,gfx_demo,,))
$(eval $(call DEFINE_GFX_APP,demo1,,))
$(eval $(call DEFINE_GFX_APP,spr_test,,))
$(eval $(call DEFINE_GFX_APP,vdpview,,))
$(eval $(call DEFINE_GFX_APP,raster,,))
$(eval $(call DEFINE_GFX_APP,ekakiuta,,))

# vbzview は DBG_OBJ を追加
programs/apps/vbzview.o: programs/apps/vbzview.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/vbzview.elf: build/app.ld $(CRT0_OBJ) programs/apps/vbzview.o $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) $(DBG_OBJ) programs/apps/vbzview.o $(GFX_OBJ) -lc -lgcc

vbzview: $(CRT0_OBJ) programs/apps/vbzview.bin

# mdview
programs/apps/mdview.o: programs/apps/mdview.c programs/libos32md/libos32md.h programs/libos32md/md_render.h programs/libos32filer/libos32filer.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32md -Iprograms/libos32filer -c $< -o $@

programs/apps/mdview.elf: build/app.ld $(CRT0_OBJ) programs/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/mdview.o $(MDLIB_OBJ) $(FILER_OBJ) $(GFX_OBJ) $(DBG_OBJ) -lc -lgcc

mdview: $(CRT0_OBJ) programs/apps/mdview.bin

# ui_demo — microUI デモ
programs/apps/ui_demo/ui_demo.o: programs/apps/ui_demo/ui_demo.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/apps/ui_demo/ui_demo.elf: build/app.ld $(CRT0_OBJ) programs/apps/ui_demo/ui_demo.o $(GFX_OBJ) $(LIBUI_OBJ)
	$(LD) $(PROGRAM_LDFLAGS) -o $@ $(CRT0_OBJ) programs/apps/ui_demo/ui_demo.o $(GFX_OBJ) $(LIBUI_OBJ) -lc -lgcc

ui_demo: $(CRT0_OBJ) programs/apps/ui_demo/ui_demo.bin

# libos32gfx/ui.o (gfx_demo が参照)
programs/libos32gfx/ui.o: programs/libos32gfx/ui.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

# === SQLite Standalone Test ===
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

# === ベースプログラム パターンルール ===
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

# === ELF → RAW → OS32X BIN 変換 ===
programs/%.raw: programs/%.elf
	$(OBJCOPY) -O binary $< $@

# OS32Xヘッダ付加 (app.conf からAPI版/ヒープサイズを読み取る)
programs/%.bin: programs/%.raw programs/%.elf
	@_api=$$(awk '$$1 == "$*" { print $$2 }' build/app.conf); \
	_heap=$$(awk '$$1 == "$*" { print $$3 }' build/app.conf); \
	_api=$${_api:-7}; \
	_heap=$${_heap:-0}; \
	if [ "$$_heap" != "0" ]; then \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api $$_api --heap $$_heap; \
	else \
		python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api $$_api; \
	fi

# === ヘルパーツール ===
unicode_bin:
	@if [ ! -f tools/gen_unicode ]; then gcc tools/gen_unicode.c -I. -Iinclude -O2 -o tools/gen_unicode; fi
	@if [ ! -f unicode.bin ]; then ./tools/gen_unicode; fi

fep_dic:
	@if [ ! -f assets/fep.db ]; then python3 tools/fep_to_sqlite.py; fi

# ============================================================================
#  Rust プログラム ビルドルール
# ============================================================================
RUST_PROGRAMS_DIR = programs/rust
RUST_TARGET_JSON  = i686-os32-none
RUST_TARGET_DIR   = $(RUST_PROGRAMS_DIR)/target/$(RUST_TARGET_JSON)/release
RUST_KAPI_RS      = $(RUST_PROGRAMS_DIR)/os32api/src/kapi_generated.rs

# Rustバインディング自動再生成 (kapi.json変更時)
$(RUST_KAPI_RS): tools/kapi.json tools/kapi_rust_gen.py
	python3 tools/kapi_rust_gen.py

# ---------------------------------------------------------------------------
#  DEFINE_RUST_PROGRAM — Rustプログラム定義テンプレート
#  $(1) = Rustクレート名 (Cargoワークスペースメンバー名)
#  $(2) = 出力先ディレクトリ (例: programs/tests)
#  $(3) = 追加リンクOBJ (例: $(GFX_OBJ))
# ---------------------------------------------------------------------------
define DEFINE_RUST_PROGRAM
$(RUST_TARGET_DIR)/lib$(1).a: FORCE $(RUST_KAPI_RS)
	cd $(RUST_PROGRAMS_DIR) && cargo build --release -p $(1)

$(2)/$(1).elf: build/app.ld $$(CRT0_OBJ) $(RUST_TARGET_DIR)/lib$(1).a $(3)
	$$(LD) $$(PROGRAM_LDFLAGS) --allow-multiple-definition \
		-o $$@ $$(CRT0_OBJ) $(3) \
		$(RUST_TARGET_DIR)/lib$(1).a \
		-lc -lgcc

$(1)_rust: $$(CRT0_OBJ) $(2)/$(1).bin

.PHONY: $(1)_rust
endef

# --- Rustプログラム登録 ---
$(eval $(call DEFINE_RUST_PROGRAM,hello_gfx,programs/tests,$$(GFX_OBJ)))
$(eval $(call DEFINE_RUST_PROGRAM,alloc_demo,programs/tests,))
$(eval $(call DEFINE_RUST_PROGRAM,math_test_rs,programs/tests,))
$(eval $(call DEFINE_RUST_PROGRAM,font_test,programs/tests,$$(GFX_OBJ)))

# Rustクリーン
clean-rust:
	cd $(RUST_PROGRAMS_DIR) && cargo clean 2>/dev/null || true

FORCE:
.PHONY: clean-rust FORCE

# === プログラム集約ターゲット ===
programs_base: $(CRT0_OBJ) $(BASE_PROGRAMS_BIN)

edit: $(CRT0_OBJ) programs/apps/edit.bin

game: $(CRT0_OBJ) programs/apps/game.bin

programs: $(DBG_OBJ) programs_base edit game bench gfx_demo spr_test demo1 vdpview raster ekakiuta vbzview mdview cdinst bench_scale2x gfx200_test gfx_demo200 blit_test blit_test2 demo_tile tile_bench rotate_test db_test e2test sqlite_standalone math_test chem_test chem_demo map_test map_demo input_test asset_test asset_demo ecs_test ecs_demo text_test text_demo econ_test ai_test btl_test board_test evt_test inv_test hello_gfx_rust alloc_demo_rust font_test_rust

# === KAPI ヘッダ依存 ===
programs/%.o: include/os32_kapi_shared.h
$(shell find programs -name '*.o'): include/os32_kapi_shared.h

# === プログラムクリーン ===
clean-programs: clean-rust
	rm -f programs/cmds/*.o programs/cmds/*.elf programs/cmds/*.raw programs/cmds/*.bin
	rm -f programs/apps/*.o programs/apps/*.elf programs/apps/*.raw programs/apps/*.bin
	rm -f programs/apps/edit/*.o
	rm -f programs/apps/game/*.o
	rm -f programs/apps/ui_demo/*.o programs/apps/ui_demo/*.elf programs/apps/ui_demo/*.raw programs/apps/ui_demo/*.bin
	rm -f programs/tests/*.o programs/tests/*.elf programs/tests/*.raw programs/tests/*.bin
	rm -f programs/tests/bench/*.o programs/tests/bench/*.elf programs/tests/bench/*.raw programs/tests/bench/*.bin
	rm -f programs/tests/bench_scale2x/*.o programs/tests/bench_scale2x/*.elf programs/tests/bench_scale2x/*.raw programs/tests/bench_scale2x/*.bin
	rm -f programs/system/*.o programs/system/*.elf programs/system/*.raw programs/system/*.bin
	rm -f programs/crt0.o programs/crt0_c.o
	rm -f programs/shell/*.o

	rm -f programs/libos32/*.o
	rm -f programs/tests/sqlite_standalone/*.o programs/tests/sqlite_standalone/*.elf programs/tests/sqlite_standalone/*.raw programs/tests/sqlite_standalone/*.bin
	rm -f lib/lz4_prog.o
	rm -f unicode.bin tools/gen_unicode

.PHONY: programs programs_base edit game lz4_cmd cdinst bench bench_scale2x
.PHONY: gfx200_test gfx_demo200 blit_test blit_test2 rotate_test
.PHONY: demo_tile tile_bench db_test e2test sqlite_standalone math_test
.PHONY: chem_test chem_demo map_test map_demo input_test
.PHONY: asset_test asset_demo ecs_test ecs_demo text_test text_demo
.PHONY: econ_test ai_test btl_test board_test evt_test inv_test
.PHONY: gfx_demo demo1 spr_test vdpview raster ekakiuta vbzview mdview
.PHONY: unicode_bin fep_dic
.PHONY: clean-programs
