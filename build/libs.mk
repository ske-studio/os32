# ============================================================================
#  libs.mk — ユーザー空間ライブラリ定義
#
#  テンプレートマクロで各ライブラリを統一的に定義する。
#  ライブラリの追加は下の INC_<name> を 1 行足し、
#  $(eval $(call DEFINE_LIB,...)) を 1 行追加するだけで完了。
# ============================================================================

# ----------------------------------------------------------------------------
#  ライブラリごとの公開インクルードパス (自分 + 依存先の推移閉包)
#
#  以前は PROGRAM_FLAGS に全ライブラリの -I を一括で並べていたため、
#  どのライブラリからでも他の全ライブラリのヘッダが見えていた。それだと
#  「宣言されていない依存」がビルドを通ってしまい、層の逆流に気づけない
#  (実際 libos32map -> libos32tilemap がそれで潜っていた)。
#
#  ここで依存を明示し、各ライブラリ/プログラムには必要な -I だけを渡す。
#  再帰変数 (=) なので定義順は問わない。-I の重複は無害。
#  依存を足したら必ずこの表も更新すること。
# ----------------------------------------------------------------------------

# 依存なし (KAPI の共有ヘッダだけで完結する)
INC_libos32math     = -Iprograms/libos32math
INC_libos32db       = -Iprograms/libos32db
INC_libos32asset    = -Iprograms/libos32asset
INC_libos32save     = -Iprograms/libos32save
INC_libos32snd      = -Iprograms/libos32snd
INC_libos32mgx      = -Iprograms/libos32mgx -Ilib/zlib

# 描画層
INC_libos32gfx      = -Iprograms/libos32gfx $(INC_libos32math)
INC_libos32ui       = -Iprograms/libos32ui $(INC_libos32gfx)
INC_libos32filer    = -Iprograms/libos32filer $(INC_libos32gfx)
INC_libos32md       = -Iprograms/libos32md $(INC_libos32gfx)
INC_libos32tilemap  = -Iprograms/libos32tilemap $(INC_libos32gfx) $(INC_libos32asset)

# 入力・汎用ロジック
INC_libos32input    = -Iprograms/libos32input $(INC_libos32math)
INC_libos32ecs      = -Iprograms/libos32ecs $(INC_libos32math)
INC_libos32turn     = -Iprograms/libos32turn $(INC_libos32math)

# DB 駆動のドメインライブラリ
INC_libos32text     = -Iprograms/libos32text $(INC_libos32db)
INC_libos32inv      = -Iprograms/libos32inv $(INC_libos32db)
INC_libos32board    = -Iprograms/libos32board $(INC_libos32db)
INC_libos32ai       = -Iprograms/libos32ai $(INC_libos32db) $(INC_libos32math)
INC_libos32chem     = -Iprograms/libos32chem $(INC_libos32db) $(INC_libos32math)
INC_libos32econ     = -Iprograms/libos32econ $(INC_libos32db) $(INC_libos32math)
INC_libos32battle   = -Iprograms/libos32battle $(INC_libos32db) $(INC_libos32math)
INC_libos32event    = -Iprograms/libos32event $(INC_libos32ai)
INC_libos32rpg      = -Iprograms/libos32rpg $(INC_libos32battle)
# map_view.c だけが libos32tilemap を直接呼ぶ (BG への転送)
INC_libos32map      = -Iprograms/libos32map $(INC_libos32db) $(INC_libos32tilemap)

# ---------------------------------------------------------------------------
#  DEFINE_LIB — ライブラリ定義テンプレート
# ---------------------------------------------------------------------------
#  $(1) = ライブラリディレクトリ名 (例: libos32math)
#  $(2) = 追加コンパイルフラグ (通常は空。INC_$(1) で足りないときだけ使う)
#  $(3) = サブディレクトリリスト (スペース区切り, 例: draw text geom)
#
#  インクルードパスは上で宣言した $(INC_$(1)) を自動で使う。
define DEFINE_LIB
$(1)_SRC := $$(wildcard programs/$(1)/*.c) $(foreach d,$(3),$$(wildcard programs/$(1)/$(d)/*.c))
$(1)_OBJ := $$($(1)_SRC:.c=.o)
ALL_LIB_OBJ += $$($(1)_OBJ)

programs/$(1)/%.o: programs/$(1)/%.c
	$$(CC) $$(PROGRAM_FLAGS) $$(INC_$(1)) $(2) -c $$< -o $$@

$(foreach d,$(3),
programs/$(1)/$(d)/%.o: programs/$(1)/$(d)/%.c
	$$(CC) $$(PROGRAM_FLAGS) $$(INC_$(1)) $(2) -c $$< -o $$@
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
GFX_OBJ = $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ) $(LIBMATH_OBJ) lib/utf8_prog.o
ALL_LIB_OBJ += $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ)

programs/libos32gfx/%.o: programs/libos32gfx/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -Ilib -c $< -o $@

programs/libos32gfx/draw/%.o: programs/libos32gfx/draw/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -Ilib -c $< -o $@

programs/libos32gfx/text/%.o: programs/libos32gfx/text/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -Ilib -c $< -o $@

programs/libos32gfx/geom/%.o: programs/libos32gfx/geom/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32gfx) -Ilib -c $< -o $@

programs/libos32gfx/asm/%.o: programs/libos32gfx/asm/%.asm programs/libos32gfx/asm/gfx_const.inc
	$(AS) -f elf32 -Iprograms/libos32gfx/asm/ $< -o $@

lib-libos32gfx: $(GFX_OBJ)
.PHONY: lib-libos32gfx

# libos32db — SQLite ユーザー空間ライブラリ
LIBOS32DB_SRC = programs/libos32db/libos32db.c
LIBOS32DB_OBJ = $(LIBOS32DB_SRC:.c=.o)
ALL_LIB_OBJ += $(LIBOS32DB_OBJ)

programs/libos32db/%.o: programs/libos32db/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32db) -c $< -o $@

lib-libos32db: $(LIBOS32DB_OBJ)
.PHONY: lib-libos32db

# libos32chem — 化学エンジン
$(eval $(call DEFINE_LIB,libos32chem,,))
LIBCHEM_OBJ = $(libos32chem_OBJ)

# libos32map — マップ管理
$(eval $(call DEFINE_LIB,libos32map,,))
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
$(eval $(call DEFINE_LIB,libos32text,,))
LIBTEXT_OBJ = $(libos32text_OBJ)

# libos32econ — 経済エンジン
$(eval $(call DEFINE_LIB,libos32econ,,))
LIBECON_OBJ = $(libos32econ_OBJ)

# libos32ai — AI意思決定エンジン
$(eval $(call DEFINE_LIB,libos32ai,,))
LIBAI_OBJ = $(libos32ai_OBJ)

# libos32battle — ターンバトルエンジン
$(eval $(call DEFINE_LIB,libos32battle,,))
LIBBATTLE_OBJ = $(libos32battle_OBJ)

# libos32board — ボードゲームエンジン
$(eval $(call DEFINE_LIB,libos32board,,))
LIBBOARD_OBJ = $(libos32board_OBJ)

# libos32turn — 手番/週スケジューラ (DB不要)
$(eval $(call DEFINE_LIB,libos32turn,,))
LIBTURN_OBJ = $(libos32turn_OBJ)

# libos32rpg — キャラクター育成・状態・リボーン
$(eval $(call DEFINE_LIB,libos32rpg,,))
LIBRPG_OBJ = $(libos32rpg_OBJ)

# libos32save — セーブデータ管理 (DB不要)
$(eval $(call DEFINE_LIB,libos32save,,))
LIBSAVE_OBJ = $(libos32save_OBJ)

# libos32event — イベントスケジューラ
$(eval $(call DEFINE_LIB,libos32event,,))
LIBEVENT_OBJ = $(libos32event_OBJ)

# libos32inv — インベントリ・装備・ショップエンジン
$(eval $(call DEFINE_LIB,libos32inv,,))
LIBINV_OBJ = $(libos32inv_OBJ)

# libos32tilemap — タイルマップエンジン (ASMソース含む)
TILEMAP_SRC = $(wildcard programs/libos32tilemap/*.c)
TILEMAP_ASM_SRC = $(wildcard programs/libos32tilemap/*.asm)
TILEMAP_ASM_OBJ = $(TILEMAP_ASM_SRC:.asm=.o)
TILEMAP_OBJ = $(TILEMAP_SRC:.c=.o) $(TILEMAP_ASM_OBJ)
ALL_LIB_OBJ += $(TILEMAP_OBJ)

programs/libos32tilemap/%.o: programs/libos32tilemap/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32tilemap) -c $< -o $@

programs/libos32tilemap/%.o: programs/libos32tilemap/%.asm
	$(AS) -f elf32 $< -o $@

lib-libos32tilemap: $(TILEMAP_OBJ)
.PHONY: lib-libos32tilemap

# zlib inflate (展開のみ) — lib/zlib/ に無改変で取り込んだ zlib 公式ソース。
#   ゲスト側は展開だけを行うので deflate/gzip/crc32 は取り込んでいない。
#   -DNO_GZIP で gzip ストリーム対応を、-DZ_SOLO で libc 依存部を落としている。
#   Z_SOLO を付けると zlib が malloc を直接呼ばなくなるので、
#   アロケータは programs/libos32mgx/mgx_decode.c が z_stream に渡す。
#   詳細と出所は lib/zlib/README.OS32 を参照。
ZLIB_DIR = lib/zlib
ZLIB_INFLATE_OBJ = $(ZLIB_DIR)/inflate_prog.o $(ZLIB_DIR)/inffast_prog.o \
                   $(ZLIB_DIR)/inftrees_prog.o $(ZLIB_DIR)/adler32_prog.o \
                   $(ZLIB_DIR)/zutil_prog.o

$(ZLIB_DIR)/%_prog.o: $(ZLIB_DIR)/%.c
	$(CC) $(PROGRAM_FLAGS) -Os -DNO_GZIP -DZ_SOLO -I$(ZLIB_DIR) -c $< -o $@

ALL_LIB_OBJ += $(ZLIB_INFLATE_OBJ)

# libos32mgx — MGX (漫画専用グレースケール画像形式) デコーダ
$(eval $(call DEFINE_LIB,libos32mgx,,))
LIBMGX_OBJ = $(libos32mgx_OBJ) $(ZLIB_INFLATE_OBJ)

# libos32snd — サウンドライブラリ
LIBSND_OBJ = programs/libos32snd/libos32snd.o

programs/libos32snd/libos32snd.o: programs/libos32snd/libos32snd.c programs/libos32snd/libos32snd.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32snd) -c $< -o $@

ALL_LIB_OBJ += $(LIBSND_OBJ)

# libos32md — Markdownパーサー + レンダラー
programs/libos32md/md_parse.o: programs/libos32md/md_parse.c programs/libos32md/libos32md.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32md) -c $< -o $@

programs/libos32md/md_render.o: programs/libos32md/md_render.c programs/libos32md/md_render.h programs/libos32md/libos32md.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32md) $(INC_libos32filer) -c $< -o $@

MDLIB_OBJ = programs/libos32md/md_parse.o programs/libos32md/md_render.o
ALL_LIB_OBJ += $(MDLIB_OBJ)

# libos32filer — GFXファイラーライブラリ + TVRAM描画
programs/libos32filer/filer_core.o: programs/libos32filer/filer_core.c programs/libos32filer/libos32filer.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32filer) -c $< -o $@

programs/libos32filer/filer_draw.o: programs/libos32filer/filer_draw.c programs/libos32filer/filer_draw.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32filer) -c $< -o $@

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
	rm -f programs/libos32mgx/*.o lib/zlib/*.o

.PHONY: libs clean-libs
