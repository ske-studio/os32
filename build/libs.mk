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

$(LIBDIR)/$(1).a: $$($(1)_OBJ)
	@rm -f $$@
	$$(AR) rcs $$@ $$^

lib-$(1): $(LIBDIR)/$(1).a

.PHONY: lib-$(1)
endef

# === 全ライブラリOBJ集約変数 (初期化) ===
ALL_LIB_OBJ :=

# === 各ライブラリの登録 ===
# libos32math — 整数数学ライブラリ (最も基底)
$(eval $(call DEFINE_LIB,libos32math,,))
LIBMATH_OBJ = $(LIBDIR)/libos32math.a

# libos32gfx — グラフィックスライブラリ (ASMソース含む, 特殊処理)
GFX_SRC = $(wildcard programs/libos32gfx/*.c) \
          $(wildcard programs/libos32gfx/draw/*.c) \
          $(wildcard programs/libos32gfx/text/*.c) \
          $(wildcard programs/libos32gfx/geom/*.c)
ASM_GFX_SRC = $(wildcard programs/libos32gfx/asm/*.asm)
ASM_GFX_OBJ = $(ASM_GFX_SRC:.asm=.o)
# lib/utf8_prog.o もここに入れる。ユーザー空間で utf8 を引くのは gfx の
# 文字描画 (lconsole.c / gfx_kcg.c) だけなので、gfx の一部として配る。
GFX_ARCHIVE_OBJ = $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ) lib/utf8_prog.o
GFX_OBJ = $(LIBDIR)/libos32gfx.a $(LIBMATH_OBJ)
ALL_LIB_OBJ += $(GFX_SRC:.c=.o) $(ASM_GFX_OBJ)

$(LIBDIR)/libos32gfx.a: $(GFX_ARCHIVE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

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

lib-libos32gfx: $(LIBDIR)/libos32gfx.a
.PHONY: lib-libos32gfx

# libos32db — SQLite ユーザー空間ライブラリ
LIBOS32DB_SRC = programs/libos32db/libos32db.c
LIBOS32DB_ARCHIVE_OBJ = $(LIBOS32DB_SRC:.c=.o)
LIBOS32DB_OBJ = $(LIBDIR)/libos32db.a
ALL_LIB_OBJ += $(LIBOS32DB_ARCHIVE_OBJ)

$(LIBDIR)/libos32db.a: $(LIBOS32DB_ARCHIVE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

programs/libos32db/%.o: programs/libos32db/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32db) -c $< -o $@

lib-libos32db: $(LIBDIR)/libos32db.a
.PHONY: lib-libos32db

# libos32chem — 化学エンジン
$(eval $(call DEFINE_LIB,libos32chem,,))
LIBCHEM_OBJ = $(LIBDIR)/libos32chem.a

# libos32map — マップ管理
$(eval $(call DEFINE_LIB,libos32map,,))
LIBMAP_OBJ = $(LIBDIR)/libos32map.a

# libos32input — 入力抽象化
$(eval $(call DEFINE_LIB,libos32input,,))
LIBINPUT_OBJ = $(LIBDIR)/libos32input.a

# libos32ui — microUI (rxi) OS32移植版
$(eval $(call DEFINE_LIB,libos32ui,,))
LIBUI_OBJ = $(LIBDIR)/libos32ui.a

# libos32asset — アセット管理
$(eval $(call DEFINE_LIB,libos32asset,,))
LIBASSET_OBJ = $(LIBDIR)/libos32asset.a

# libos32ecs — ECS ゲームオブジェクト管理
$(eval $(call DEFINE_LIB,libos32ecs,,))
LIBECS_OBJ = $(LIBDIR)/libos32ecs.a

# libos32text — テキスト管理
$(eval $(call DEFINE_LIB,libos32text,,))
LIBTEXT_OBJ = $(LIBDIR)/libos32text.a

# libos32econ — 経済エンジン
$(eval $(call DEFINE_LIB,libos32econ,,))
LIBECON_OBJ = $(LIBDIR)/libos32econ.a

# libos32ai — AI意思決定エンジン
$(eval $(call DEFINE_LIB,libos32ai,,))
LIBAI_OBJ = $(LIBDIR)/libos32ai.a

# libos32battle — ターンバトルエンジン
$(eval $(call DEFINE_LIB,libos32battle,,))
LIBBATTLE_OBJ = $(LIBDIR)/libos32battle.a

# libos32board — ボードゲームエンジン
$(eval $(call DEFINE_LIB,libos32board,,))
LIBBOARD_OBJ = $(LIBDIR)/libos32board.a

# libos32turn — 手番/週スケジューラ (DB不要)
$(eval $(call DEFINE_LIB,libos32turn,,))
LIBTURN_OBJ = $(LIBDIR)/libos32turn.a

# libos32rpg — キャラクター育成・状態・リボーン
$(eval $(call DEFINE_LIB,libos32rpg,,))
LIBRPG_OBJ = $(LIBDIR)/libos32rpg.a

# libos32save — セーブデータ管理 (DB不要)
$(eval $(call DEFINE_LIB,libos32save,,))
LIBSAVE_OBJ = $(LIBDIR)/libos32save.a

# libos32event — イベントスケジューラ
$(eval $(call DEFINE_LIB,libos32event,,))
LIBEVENT_OBJ = $(LIBDIR)/libos32event.a

# libos32inv — インベントリ・装備・ショップエンジン
$(eval $(call DEFINE_LIB,libos32inv,,))
LIBINV_OBJ = $(LIBDIR)/libos32inv.a

# libos32tilemap — タイルマップエンジン (ASMソース含む)
TILEMAP_SRC = $(wildcard programs/libos32tilemap/*.c)
TILEMAP_ASM_SRC = $(wildcard programs/libos32tilemap/*.asm)
TILEMAP_ASM_OBJ = $(TILEMAP_ASM_SRC:.asm=.o)
TILEMAP_ARCHIVE_OBJ = $(TILEMAP_SRC:.c=.o) $(TILEMAP_ASM_OBJ)
TILEMAP_OBJ = $(LIBDIR)/libos32tilemap.a
ALL_LIB_OBJ += $(TILEMAP_ARCHIVE_OBJ)

$(LIBDIR)/libos32tilemap.a: $(TILEMAP_ARCHIVE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

programs/libos32tilemap/%.o: programs/libos32tilemap/%.c
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32tilemap) -c $< -o $@

programs/libos32tilemap/%.o: programs/libos32tilemap/%.asm
	$(AS) -f elf32 $< -o $@

lib-libos32tilemap: $(LIBDIR)/libos32tilemap.a
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

# vendored zlib は OS32 のライブラリと混ぜず独立したアーカイブにする
# (出所とライセンスの見通しのため。lib/zlib/README.OS32 を参照)
$(LIBDIR)/libzinflate.a: $(ZLIB_INFLATE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

# libos32mgx — MGX (漫画専用グレースケール画像形式) デコーダ
$(eval $(call DEFINE_LIB,libos32mgx,,))
LIBMGX_OBJ = $(LIBDIR)/libos32mgx.a $(LIBDIR)/libzinflate.a

# libos32snd — サウンドライブラリ
LIBSND_ARCHIVE_OBJ = programs/libos32snd/libos32snd.o
LIBSND_OBJ = $(LIBDIR)/libos32snd.a

$(LIBDIR)/libos32snd.a: $(LIBSND_ARCHIVE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

programs/libos32snd/libos32snd.o: programs/libos32snd/libos32snd.c programs/libos32snd/libos32snd.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32snd) -c $< -o $@

ALL_LIB_OBJ += $(LIBSND_ARCHIVE_OBJ)

# libos32md — Markdownパーサー + レンダラー
programs/libos32md/md_parse.o: programs/libos32md/md_parse.c programs/libos32md/libos32md.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32md) -c $< -o $@

programs/libos32md/md_render.o: programs/libos32md/md_render.c programs/libos32md/md_render.h programs/libos32md/libos32md.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32md) $(INC_libos32filer) -c $< -o $@

MDLIB_ARCHIVE_OBJ = programs/libos32md/md_parse.o programs/libos32md/md_render.o
MDLIB_OBJ = $(LIBDIR)/libos32md.a
ALL_LIB_OBJ += $(MDLIB_ARCHIVE_OBJ)

$(LIBDIR)/libos32md.a: $(MDLIB_ARCHIVE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

# libos32filer — GFXファイラーライブラリ + TVRAM描画
programs/libos32filer/filer_core.o: programs/libos32filer/filer_core.c programs/libos32filer/libos32filer.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32filer) -c $< -o $@

programs/libos32filer/filer_draw.o: programs/libos32filer/filer_draw.c programs/libos32filer/filer_draw.h
	$(CC) $(PROGRAM_FLAGS) $(INC_libos32filer) -c $< -o $@

# filer_core.o (GFX版, filer_*) と filer_draw.o (TVRAM版, fldraw_*) を
# 1 つのアーカイブに入れる。両者はシンボルが重ならないので、ld は
# 参照された方のメンバだけを引き込む。
FILER_ARCHIVE_OBJ = programs/libos32filer/filer_core.o programs/libos32filer/filer_draw.o
FILER_OBJ = $(LIBDIR)/libos32filer.a
FILER_DRAW_OBJ = $(LIBDIR)/libos32filer.a
ALL_LIB_OBJ += $(FILER_ARCHIVE_OBJ)

$(LIBDIR)/libos32filer.a: $(FILER_ARCHIVE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

# === ライブラリ全ビルドターゲット ===
# 全ライブラリのアーカイブを作る。SDK 切り出し時はこの LIBDIR がそのまま
# sysroot の lib/ になる。
ALL_LIB_ARCHIVES = $(LIBDIR)/libos32math.a $(LIBDIR)/libos32gfx.a \
                   $(LIBDIR)/libos32db.a $(LIBDIR)/libos32asset.a \
                   $(LIBDIR)/libos32save.a $(LIBDIR)/libos32snd.a \
                   $(LIBDIR)/libos32mgx.a $(LIBDIR)/libzinflate.a \
                   $(LIBDIR)/libos32ui.a $(LIBDIR)/libos32filer.a \
                   $(LIBDIR)/libos32md.a $(LIBDIR)/libos32tilemap.a \
                   $(LIBDIR)/libos32input.a $(LIBDIR)/libos32ecs.a \
                   $(LIBDIR)/libos32turn.a $(LIBDIR)/libos32text.a \
                   $(LIBDIR)/libos32inv.a $(LIBDIR)/libos32board.a \
                   $(LIBDIR)/libos32ai.a $(LIBDIR)/libos32chem.a \
                   $(LIBDIR)/libos32econ.a $(LIBDIR)/libos32battle.a \
                   $(LIBDIR)/libos32event.a $(LIBDIR)/libos32rpg.a \
                   $(LIBDIR)/libos32map.a

libs: $(ALL_LIB_ARCHIVES)

# === ライブラリクリーン ===
clean-libs:
	rm -f $(LIBDIR)/*.a
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
