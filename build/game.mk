# ============================================================================
#  game.mk — ゲーム固有のデータ生成
#
#  ゲームの SQLite マスタデータは game/data/ のスクリプトから毎回生成する
#  生成物なので、git では追跡しない (game/build/db/ は .gitignore)。
#  バイナリ差分がレビューに出てこなくなるのと、データの正が
#  スクリプト側にあることがはっきりするのが狙い。
# ============================================================================

GAME_DB_DIR = game/build/db

# 生成物の一覧。tools/check_manifests.py と game/deploy.yaml の参照先。
GAME_DBS = $(GAME_DB_DIR)/ai.db      $(GAME_DB_DIR)/battle.db \
           $(GAME_DB_DIR)/board.db   $(GAME_DB_DIR)/board_test.db \
           $(GAME_DB_DIR)/chem.db    $(GAME_DB_DIR)/econ.db \
           $(GAME_DB_DIR)/econ_test.db $(GAME_DB_DIR)/events.db \
           $(GAME_DB_DIR)/events_test.db $(GAME_DB_DIR)/items.db \
           $(GAME_DB_DIR)/map.db     $(GAME_DB_DIR)/rpg.db \
           $(GAME_DB_DIR)/text.db

# board.db は gen_worldmap.py が正 (gen_board_db.py --game は旧世代の
# 小さい盤面を作るので使わない。--test 用の board_test.db だけを使う)。
$(GAME_DB_DIR)/board.db: game/data/gen_worldmap.py
	@mkdir -p $(GAME_DB_DIR)
	python3 $< --write

$(GAME_DB_DIR)/board_test.db: game/data/gen_board_db.py
	@mkdir -p $(GAME_DB_DIR)
	python3 $< --test

$(GAME_DB_DIR)/econ.db: game/data/econ_db_init.py
	@mkdir -p $(GAME_DB_DIR)
	python3 $< --game

$(GAME_DB_DIR)/econ_test.db: game/data/econ_db_init.py
	@mkdir -p $(GAME_DB_DIR)
	python3 $< --test

$(GAME_DB_DIR)/events.db: game/data/gen_events_db.py
	@mkdir -p $(GAME_DB_DIR)
	python3 $< --game

$(GAME_DB_DIR)/events_test.db: game/data/gen_events_db.py
	@mkdir -p $(GAME_DB_DIR)
	python3 $< --test

$(GAME_DB_DIR)/ai.db:     game/data/gen_ai_db.py     ; @mkdir -p $(GAME_DB_DIR); python3 $<
$(GAME_DB_DIR)/battle.db: game/data/gen_battle_db.py ; @mkdir -p $(GAME_DB_DIR); python3 $<
$(GAME_DB_DIR)/items.db:  game/data/gen_items_db.py  ; @mkdir -p $(GAME_DB_DIR); python3 $<
$(GAME_DB_DIR)/rpg.db:    game/data/gen_rpg_db.py    ; @mkdir -p $(GAME_DB_DIR); python3 $<
$(GAME_DB_DIR)/chem.db:   game/data/chem_db_init.py  ; @mkdir -p $(GAME_DB_DIR); python3 $<
$(GAME_DB_DIR)/map.db:    game/data/map_db_init.py   ; @mkdir -p $(GAME_DB_DIR); python3 $<
$(GAME_DB_DIR)/text.db:   game/data/text_db_init.py  ; @mkdir -p $(GAME_DB_DIR); python3 $<

game-data: $(GAME_DBS)
	@echo "=== ゲームDB $(words $(GAME_DBS)) 本 ($(GAME_DB_DIR)) ==="

clean-game-data:
	rm -rf game/build

.PHONY: game-data clean-game-data
