# ============================================================================
#  game.mk — ゲームのビルド (game/Makefile への委譲)
#
#  ゲームは SDK だけを使ってビルドする。OS 側のソースツリーを直接参照
#  しないので、game/ をそのまま別リポジトリ (os32-game) へ切り出せる。
#  ビルド定義の実体は game/Makefile と game/data/data.mk にあり、
#  ここは呼び出すだけ。定義を二重に持つと必ず食い違うので増やさないこと。
# ============================================================================

GAME_DIR    = game
GAME_MAKE   = $(MAKE) -C $(GAME_DIR) OS32_SDK=$(abspath $(SDK_OUT)) CROSS_DIR=$(CROSS_DIR)
GAME_DB_DIR = $(GAME_DIR)/build/db

# ゲームのビルドには SDK が要る (make sdk が先)。
game: sdk
	$(GAME_MAKE) all

game-data:
	$(GAME_MAKE) data

clean-game:
	$(GAME_MAKE) clean

.PHONY: game game-data clean-game
