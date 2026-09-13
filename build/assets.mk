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

# --- 設定レジストリの初期値マスタ (settings.db) ---
# 正典は assets/settings/defaults.tsv (人が読み書きする側)。生成した DB は
# インストール媒体 (FDD / CD) だけが持ち、既存システムには tsv を通常配備して
# `cfg init` (S2) が明示的に生成する (TASK_S0 §3)。
# FORCE 依存 = ビルド毎に必ず作り直す (ユーザー決裁)。生成は決定的
# (同じ tsv + 同じ epoch → 同じバイト列) なので、毎回作っても媒体の中身は動かない。
SETTINGS_TSV = assets/settings/defaults.tsv
SETTINGS_DB  = $(BUILD_OUT)/settings.db

$(SETTINGS_DB): $(SETTINGS_TSV) tools/mk_settings_db.py FORCE
	@mkdir -p $(dir $@)
	python3 tools/mk_settings_db.py --tsv $(SETTINGS_TSV) --out $@

# 版 2 の試験 fixture (票 S4 の受入 G5 = VERSION 状態の gshell)。通常配備は
# /etc/settings.db* を保護してスキップするので、保護対象でない名前で配備し、
# ゲストで `cp /etc/settings.v2.fixture /etc/settings.db` して使う。
SETTINGS_V2_FIXTURE = $(BUILD_OUT)/settings.v2.fixture

$(SETTINGS_V2_FIXTURE): $(SETTINGS_TSV) tools/mk_settings_db.py FORCE
	@mkdir -p $(dir $@)
	python3 tools/mk_settings_db.py --tsv $(SETTINGS_TSV) --out $@ --schema-version 2

# 配備に必要な最小限。make all はこれに依存する。
ASSETS_DEPLOYED = $(FONT_DIR)/ipaexg16.kcgfont $(FONT_DIR)/ipaexg_subset.ttf \
                  assets/fep.db

# 開発時に使うものも含めた全部。
# settings.db は通常配備の対象ではない (媒体だけが持つ) ので ASSETS_DEPLOYED
# には入れず、ここと `all` / 媒体ターゲットから引く。
ASSETS_ALL = $(ASSETS_DEPLOYED) $(FONT_DIR)/ipaexm_subset.ttf \
             assets/fep_s.db assets/fep_l.db assets/fep.dic \
             $(SETTINGS_DB) $(SETTINGS_V2_FIXTURE)

# `all` からも直接引く (媒体ターゲットの依存とは別に、単体で必ず出来ていること)。
all: $(SETTINGS_DB) $(SETTINGS_V2_FIXTURE)

assets-deployed: $(ASSETS_DEPLOYED)
	@echo "=== 配備用アセット $(words $(ASSETS_DEPLOYED)) 件 ==="

assets-all: $(ASSETS_ALL)
	@echo "=== 派生アセット $(words $(ASSETS_ALL)) 件 ==="

clean-assets:
	rm -f $(ASSETS_ALL)

.PHONY: assets-deployed assets-all clean-assets
