# ============================================================================
#  data.mk — ゲームのマスタデータ生成
#
#  SQLite のマスタデータは data/ のスクリプトから毎回生成する生成物なので、
#  git では追跡しない (build/ は .gitignore)。バイナリ差分がレビューに
#  出てこなくなるのと、データの正がスクリプト側にあることがはっきりする。
#
#  game/Makefile から include される。DB_DIR は呼び出し側が定義する。
# ============================================================================


# 生成物の一覧。tools/check_manifests.py と game/deploy.yaml の参照先。
GAME_DBS = $(DB_DIR)/ai.db      $(DB_DIR)/battle.db \
           $(DB_DIR)/board.db   $(DB_DIR)/board_test.db \
           $(DB_DIR)/chem.db    $(DB_DIR)/econ.db \
           $(DB_DIR)/econ_test.db $(DB_DIR)/events.db \
           $(DB_DIR)/events_test.db $(DB_DIR)/items.db \
           $(DB_DIR)/map.db     $(DB_DIR)/rpg.db \
           $(DB_DIR)/text.db

# board.db は gen_worldmap.py が正 (gen_board_db.py --game は旧世代の
# 小さい盤面を作るので使わない。--test 用の board_test.db だけを使う)。
$(DB_DIR)/board.db: data/gen_worldmap.py
	@mkdir -p $(DB_DIR)
	python3 $< --write

$(DB_DIR)/board_test.db: data/gen_board_db.py
	@mkdir -p $(DB_DIR)
	python3 $< --test

$(DB_DIR)/econ.db: data/econ_db_init.py
	@mkdir -p $(DB_DIR)
	python3 $< --game

$(DB_DIR)/econ_test.db: data/econ_db_init.py
	@mkdir -p $(DB_DIR)
	python3 $< --test

$(DB_DIR)/events.db: data/gen_events_db.py
	@mkdir -p $(DB_DIR)
	python3 $< --game

$(DB_DIR)/events_test.db: data/gen_events_db.py
	@mkdir -p $(DB_DIR)
	python3 $< --test

$(DB_DIR)/ai.db:     data/gen_ai_db.py     ; @mkdir -p $(DB_DIR); python3 $<
$(DB_DIR)/battle.db: data/gen_battle_db.py ; @mkdir -p $(DB_DIR); python3 $<
$(DB_DIR)/items.db:  data/gen_items_db.py  ; @mkdir -p $(DB_DIR); python3 $<
$(DB_DIR)/rpg.db:    data/gen_rpg_db.py    ; @mkdir -p $(DB_DIR); python3 $<
$(DB_DIR)/chem.db:   data/chem_db_init.py  ; @mkdir -p $(DB_DIR); python3 $<
$(DB_DIR)/map.db:    data/map_db_init.py   ; @mkdir -p $(DB_DIR); python3 $<
$(DB_DIR)/text.db:   data/text_db_init.py  ; @mkdir -p $(DB_DIR); python3 $<

data: $(GAME_DBS)
	@echo "=== ゲームDB $(words $(GAME_DBS)) 本 ($(DB_DIR)) ==="
