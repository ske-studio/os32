# ============================================================================
#  assets.mk — ホスト側アセットの生成
#
#  assets/ には性質の違う 3 種類が混ざっている:
#
#    上流       第三者の配布物そのもの。再生成できないので git で追跡する。
#               fonts/ipaexg.ttf, fonts/ipaexm.ttf, ipadic/*.csv
#    設定       人が書くもの。git で追跡する。
#               filetypes, profile, profile_fdd, joyo_kanji.txt
#    派生物     上流から生成できるもの。git では追跡せず、ここで作る。
#               fonts/*_subset.ttf, fonts/*.kcgfont, fep*.db, fep.dic
#
#  派生物を追跡しないのは、履歴上位の巨大 blob の大半がこれだったため。
#  生成は数秒で終わる (FEP 辞書 5.8MB で 1 秒未満)。
# ============================================================================

FONT_DIR   = assets/fonts
IPADIC_DIR = assets/ipadic

# --- サブセット TTF (JIS X 0208 の範囲だけ残す。約 45% 削減) ---
$(FONT_DIR)/%_subset.ttf: $(FONT_DIR)/%.ttf tools/subset_font.py
	python3 tools/subset_font.py $< $@

# --- 16px ビットマップフォント (カーネルが /sys/font/default.kcgfont で読む) ---
# 本文用はゴシック。明朝は 16x16 だと細い横画が飛ぶ (CLAUDE.md の Known Gotchas)。
$(FONT_DIR)/ipaexg16.kcgfont: $(FONT_DIR)/ipaexg.ttf tools/gen_font16.py
	python3 tools/gen_font16.py $< $@

# --- FEP (かな漢字変換) 辞書 ---
# M がゲストに載る既定。S/L はコスト閾値違いで、kernel/ime.c が
# /db/fep_s.db /db/fep_l.db として参照する (現在は配備していない)。
assets/fep.db: $(IPADIC_DIR)/Noun.csv tools/fep_to_sqlite.py
	python3 tools/fep_to_sqlite.py -i $(IPADIC_DIR) -o $@

assets/fep_s.db: $(IPADIC_DIR)/Noun.csv tools/fep_to_sqlite.py
	python3 tools/fep_to_sqlite.py --size S

assets/fep_l.db: $(IPADIC_DIR)/Noun.csv tools/fep_to_sqlite.py
	python3 tools/fep_to_sqlite.py --size L

# --- 旧形式のバイナリ辞書 (現在ゲストは使っていない) ---
assets/fep.dic: $(IPADIC_DIR)/Noun.csv tools/fep_compiler.py
	python3 tools/fep_compiler.py -o $@

# 配備に必要な最小限。make all はこれに依存する。
ASSETS_DEPLOYED = $(FONT_DIR)/ipaexg16.kcgfont $(FONT_DIR)/ipaexg_subset.ttf \
                  assets/fep.db

# 開発時に使うものも含めた全部。
ASSETS_ALL = $(ASSETS_DEPLOYED) $(FONT_DIR)/ipaexm_subset.ttf \
             assets/fep_s.db assets/fep_l.db assets/fep.dic

assets-deployed: $(ASSETS_DEPLOYED)
	@echo "=== 配備用アセット $(words $(ASSETS_DEPLOYED)) 件 ==="

assets-all: $(ASSETS_ALL)
	@echo "=== 派生アセット $(words $(ASSETS_ALL)) 件 ==="

clean-assets:
	rm -f $(ASSETS_ALL)

.PHONY: assets-deployed assets-all clean-assets
