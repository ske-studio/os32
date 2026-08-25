# ============================================================================
#  libs.mk — ユーザー空間ライブラリ定義
#
#  テンプレートマクロで各ライブラリを統一的に定義する。
#  ライブラリの追加は $(eval $(call DEFINE_LIB,...)) を1行追加するだけで完了。
# ============================================================================

# ---------------------------------------------------------------------------
#  DEFINE_LIB — ライブラリ定義テンプレート
#  $(1) = ライブラリディレクトリ名 (例: libos32math)
#  $(2) = 追加コンパイルフラグ (例: -Iprograms/libos32db)
#  $(3) = サブディレクトリリスト (スペース区切り, 例: draw text geom)
# ---------------------------------------------------------------------------
define DEFINE_LIB
$(1)_SRC := $$(wildcard programs/$(1)/*.c) $(foreach d,$(3),$$(wildcard programs/$(1)/$(d)/*.c))
$(1)_OBJ := $$($(1)_SRC:.c=.o)
ALL_LIB_OBJ += $$($(1)_OBJ)

programs/$(1)/%.o: programs/$(1)/%.c
	$$(CC) $$(PROGRAM_FLAGS) $(2) -c $$< -o $$@

$(foreach d,$(3),
programs/$(1)/$(d)/%.o: programs/$(1)/$(d)/%.c
	$$(CC) $$(PROGRAM_FLAGS) $(2) -c $$< -o $$@
)

lib-$(1): $$($(1)_OBJ)

.PHONY: lib-$(1)
endef

# === 全ライブラリOBJ集約変数 (初期化) ===
ALL_LIB_OBJ :=

# === 各ライブラリの登録 ===
# libos32math — 整数数学ライブラリ (最も基底)
$(eval $(call DEFINE_LIB,libos32math,,))
LIBMATH_OBJ = $(libos32math_OBJ)

# libos32gfx — グラフィックスライブラリ (ASMソース含む, 特殊処理)
GFX_SRC = $(wildcard programs/libos32gfx/*.c) \
          $(wildcard programs/libos32gfx/draw/*.c) \
          $(wildcard programs/libos32gfx/text/*.c) \
          $(wildcard programs/libos32gfx/geom/*.c)
ASM_GFX_SRC = $(wildcard programs/libos32gfx/asm/*.asm)
ASM_GFX_OBJ = $(ASM_GFX_SRC:.asm=.o)
GFX_OBJ = $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ) $(LIBMATH_OBJ) lib/utf8.o
ALL_LIB_OBJ += $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ)

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

lib-libos32gfx: $(GFX_OBJ)
.PHONY: lib-libos32gfx

# libos32db — SQLite ユーザー空間ライブラリ
LIBOS32DB_SRC = programs/libos32db/libos32db.c
LIBOS32DB_OBJ = $(LIBOS32DB_SRC:.c=.o)
ALL_LIB_OBJ += $(LIBOS32DB_OBJ)

programs/libos32db/%.o: programs/libos32db/%.c
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32db -c $< -o $@

lib-libos32db: $(LIBOS32DB_OBJ)
.PHONY: lib-libos32db

# libos32chem — 化学エンジン
$(eval $(call DEFINE_LIB,libos32chem,-Iprograms/libos32db,))
LIBCHEM_OBJ = $(libos32chem_OBJ)

# libos32map — マップ管理
$(eval $(call DEFINE_LIB,libos32map,-Iprograms/libos32db,))
LIBMAP_OBJ = $(libos32map_OBJ)

# libos32input — 入力抽象化
$(eval $(call DEFINE_LIB,libos32input,,))
LIBINPUT_OBJ = $(libos32input_OBJ)

# libos32ui — microUI (rxi) OS32移植版
$(eval $(call DEFINE_LIB,libos32ui,,))
LIBUI_OBJ = $(libos32ui_OBJ)

# libos32asset — アセット管理
$(eval $(call DEFINE_LIB,libos32asset,,))
LIBASSET_OBJ = $(libos32asset_OBJ)

# libos32ecs — ECS ゲームオブジェクト管理
$(eval $(call DEFINE_LIB,libos32ecs,,))
LIBECS_OBJ = $(libos32ecs_OBJ)

# libos32text — テキスト管理
$(eval $(call DEFINE_LIB,libos32text,-Iprograms/libos32db,))
LIBTEXT_OBJ = $(libos32text_OBJ)

# libos32econ — 経済エンジン
$(eval $(call DEFINE_LIB,libos32econ,-Iprograms/libos32db,))
LIBECON_OBJ = $(libos32econ_OBJ)

# libos32ai — AI意思決定エンジン
$(eval $(call DEFINE_LIB,libos32ai,-Iprograms/libos32db,))
LIBAI_OBJ = $(libos32ai_OBJ)

# libos32battle — ターンバトルエンジン
$(eval $(call DEFINE_LIB,libos32battle,-Iprograms/libos32db,))
LIBBATTLE_OBJ = $(libos32battle_OBJ)

# libos32board — ボードゲームエンジン
$(eval $(call DEFINE_LIB,libos32board,-Iprograms/libos32db,))
LIBBOARD_OBJ = $(libos32board_OBJ)

# libos32turn — 手番/週スケジューラ (DB不要)
$(eval $(call DEFINE_LIB,libos32turn,,))
LIBTURN_OBJ = $(libos32turn_OBJ)

# libos32rpg — キャラクター育成・状態・リボーン
$(eval $(call DEFINE_LIB,libos32rpg,-Iprograms/libos32db,))
LIBRPG_OBJ = $(libos32rpg_OBJ)

# libos32save — セーブデータ管理 (DB不要)
$(eval $(call DEFINE_LIB,libos32save,,))
LIBSAVE_OBJ = $(libos32save_OBJ)

# libos32event — イベントスケジューラ
$(eval $(call DEFINE_LIB,libos32event,-Iprograms/libos32db,))
LIBEVENT_OBJ = $(libos32event_OBJ)

# libos32inv — インベントリ・装備・ショップエンジン
$(eval $(call DEFINE_LIB,libos32inv,-Iprograms/libos32db,))
LIBINV_OBJ = $(libos32inv_OBJ)

# libos32tilemap — タイルマップエンジン (ASMソース含む)
TILEMAP_SRC = $(wildcard programs/libos32tilemap/*.c)
TILEMAP_ASM_SRC = $(wildcard programs/libos32tilemap/*.asm)
TILEMAP_ASM_OBJ = $(TILEMAP_ASM_SRC:.asm=.o)
TILEMAP_OBJ = $(TILEMAP_SRC:.c=.o) $(TILEMAP_ASM_OBJ)
ALL_LIB_OBJ += $(TILEMAP_OBJ)

programs/libos32tilemap/%.o: programs/libos32tilemap/%.c
	$(CC) $(PROGRAM_FLAGS) -c $< -o $@

programs/libos32tilemap/%.o: programs/libos32tilemap/%.asm
	$(AS) -f elf32 $< -o $@

lib-libos32tilemap: $(TILEMAP_OBJ)
.PHONY: lib-libos32tilemap

# libos32snd — サウンドライブラリ
LIBSND_OBJ = programs/libos32snd/libos32snd.o

programs/libos32snd/libos32snd.o: programs/libos32snd/libos32snd.c programs/libos32snd/libos32snd.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32snd -c $< -o $@

ALL_LIB_OBJ += $(LIBSND_OBJ)

# libos32md — Markdownパーサー + レンダラー
programs/libos32md/md_parse.o: programs/libos32md/md_parse.c programs/libos32md/libos32md.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32md -c $< -o $@

programs/libos32md/md_render.o: programs/libos32md/md_render.c programs/libos32md/md_render.h programs/libos32md/libos32md.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32md -Iprograms/libos32filer -c $< -o $@

MDLIB_OBJ = programs/libos32md/md_parse.o programs/libos32md/md_render.o
ALL_LIB_OBJ += $(MDLIB_OBJ)

# libos32filer — GFXファイラーライブラリ + TVRAM描画
programs/libos32filer/filer_core.o: programs/libos32filer/filer_core.c programs/libos32filer/libos32filer.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32filer -c $< -o $@

programs/libos32filer/filer_draw.o: programs/libos32filer/filer_draw.c programs/libos32filer/filer_draw.h
	$(CC) $(PROGRAM_FLAGS) -Iprograms/libos32filer -c $< -o $@

FILER_OBJ = programs/libos32filer/filer_core.o
FILER_DRAW_OBJ = programs/libos32filer/filer_draw.o
ALL_LIB_OBJ += $(FILER_OBJ) $(FILER_DRAW_OBJ)

# === ライブラリ全ビルドターゲット ===
libs: $(ALL_LIB_OBJ)

# === ライブラリクリーン ===
clean-libs:
	rm -f programs/libos32math/*.o programs/libos32chem/*.o programs/libos32map/*.o
	rm -f programs/libos32input/*.o programs/libos32asset/*.o programs/libos32text/*.o
	rm -f programs/libos32econ/*.o programs/libos32ai/*.o programs/libos32battle/*.o
	rm -f programs/libos32inv/*.o programs/libos32board/*.o programs/libos32event/*.o
	rm -f programs/libos32ecs/*.o programs/libos32ui/*.o
	rm -f programs/libos32gfx/*.o programs/libos32gfx/asm/*.o
	rm -f programs/libos32gfx/draw/*.o programs/libos32gfx/text/*.o programs/libos32gfx/geom/*.o
	rm -f programs/libos32tilemap/*.o
	rm -f programs/libos32db/*.o programs/libos32snd/*.o
	rm -f programs/libos32md/*.o programs/libos32filer/*.o
	rm -f programs/libos32turn/*.o programs/libos32rpg/*.o programs/libos32save/*.o

.PHONY: libs clean-libs
