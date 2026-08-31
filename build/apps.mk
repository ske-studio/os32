# ============================================================================
#  apps.mk — 標準アプリのビルド (apps/Makefile への委譲)
#
#  アプリは SDK だけを使ってビルドする。OS 側のソースツリーを直接参照
#  しないので、apps/ をそのまま別リポジトリ (os32-apps) へ切り出せる。
#  ビルド定義の実体は apps/Makefile にあり、ここは呼ぶだけ。
#  定義を二重に持つと必ず食い違うので増やさないこと。
# ============================================================================

APPS_DIR  = apps
APPS_MAKE = $(MAKE) -C $(APPS_DIR) OS32_SDK=$(abspath $(SDK_OUT)) CROSS_DIR=$(CROSS_DIR)

apps: sdk
	$(APPS_MAKE) all

# apps/<name>/<name>.bin を単体で作れるようにする。個別ホットデプロイ
# (make dp-apps/<name>/<name>) がこのパターンを要求する。
# $* = <name>/<name> なので notdir でアプリ名だけを取り出して委譲する。
apps/%.bin: sdk
	$(APPS_MAKE) $(notdir $*)

clean-apps:
	$(APPS_MAKE) clean

.PHONY: apps clean-apps
