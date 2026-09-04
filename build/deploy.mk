# ============================================================================
#  deploy.mk — デプロイターゲット
# ============================================================================

# deploy: HostDrv方式 — ビルド成果物をC:\os32にコピー (sudo不要, 再起動不要)
# ゲストOSは /host 経由で直接アクセス可能
deploy: $(BUILD_OUT)/vmkernel.lz4 programs unicode_bin
	@echo "=== HostDrv Deploy ==="
	$(HOSTDRV_DEPLOY) sync
	$(PRUNE_STALE) hostdrv $(PRUNE_FLAG)

# deploy-kernel: vmkernel.lz4 と全ビルド成果物をNHDのext2に配置
#   (NP21/W再起動が必要)。HostDrv を先に同期するので、HostDrv 側が古いまま
#   ext2 を上書きして中身を失う事故を防げる。
#   ブートローダー自体を変更した場合は deploy-boot を別途実行すること。
deploy-kernel: $(BUILD_OUT)/vmkernel.lz4
	@echo "=== Sync to HostDrv before NHD deploy ==="
	$(HOSTDRV_DEPLOY) sync
	$(NHD_DEPLOY) sync-from-hostdrv
	$(PRUNE_STALE) both $(PRUNE_FLAG)
	$(NHD_DEPLOY) deploy

# deploy-boot: ブートローダーのみNHDブート領域 (LBA 2-17) に書き込み
#   boot/loader_hdd.bin を変更した場合のみ実行する
deploy-boot: boot/loader_hdd.bin
	@echo "=== Boot Loader Deploy ==="
	$(NHD_DEPLOY) write-boot boot/loader_hdd.bin

# deploy-nhd: NHDフルデプロイ (ローダー+全ファイル)
deploy-nhd: $(BUILD_OUT)/vmkernel.lz4 programs unicode_bin
	@echo "=== NHD Deploy (using deploy.yaml) ==="
	$(NHD_DEPLOY) sync
	$(PRUNE_STALE) both $(PRUNE_FLAG)
	$(NHD_DEPLOY) deploy

# prune-stale: 配備先 (HostDrv + NHD) に残ったマニフェストに無い *.bin を掃除する。
#   配備は書くだけで消さないので、KAPI 変更後の stale バイナリ (別関数へ飛んで
#   rshell ごと沈黙する) が溜まる。deploy / deploy-nhd / deploy-kernel は既定で
#   削除まで行う (NO_PRUNE=1 で一覧だけ)。対象はシステム側ディレクトリ直下の
#   *.bin のみで、/home /data /etc には触れない。
#   例: make prune-stale            一覧だけ (dry-run)
#       make prune-stale-delete     削除 (NHD 側は次の deploy で Windows へ反映)
PRUNE_STALE = python3 tools/prune_stale.py
PRUNE_FLAG  = $(if $(NO_PRUNE),,--delete)
prune-stale:
	$(PRUNE_STALE) both
prune-stale-delete:
	$(PRUNE_STALE) both --delete

# hotdeploy: 個別バイナリのビルド → ホットデプロイ (再起動不要)
#   例: make hotdeploy FILE=apps/hello32/hello32.bin
#       make hotdeploy FILE=userland/shell.bin
#   配備先はマニフェストから解決する。GUEST= で明示指定もできる。
#
#   旧 `dp-%` パターンルールは GNU make の仕様上成立していなかった。
#   ターゲットパターンにスラッシュが無い場合、make はディレクトリ部を
#   除いたファイル名だけを照合するため `dp-apps/x/x` はどの規則にも当たらない。
hotdeploy:
	@test -n "$(FILE)" || { \
	  echo "usage: make hotdeploy FILE=<path/to/x.bin> [GUEST=/usr/bin/x.bin]"; \
	  exit 1; }
	$(MAKE) $(FILE)
	@echo "=== Hot Deploy: $(FILE) ==="
	python3 tools/hotdeploy.py $(FILE) $(GUEST)

# nhd-mount: NHDのext2パーティションをマウント
nhd-mount:
	$(NHD_DEPLOY) mount

# nhd-umount: NHDのext2パーティションをアンマウント
nhd-umount:
	$(NHD_DEPLOY) umount

# nhd-pull: Windows側NHDを build/nhd/ に取り込む (フォーマットしない)
#   deploy-nhd はローカル NHD が無ければ自動で pull するので通常は不要
nhd-pull:
	$(NHD_DEPLOY) pull

# nhd-init: 初回セットアップ (Windows側NHDコピー + フォーマット + マウント)
#   フォーマットするのでゲスト側で作られたデータは消える
nhd-init:
	$(NHD_DEPLOY) init

.PHONY: deploy deploy-kernel deploy-boot deploy-nhd hotdeploy nhd-mount nhd-umount nhd-pull nhd-init prune-stale prune-stale-delete
