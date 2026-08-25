# ============================================================================
#  deploy.mk — デプロイターゲット
# ============================================================================

# deploy: HostDrv方式 — ビルド成果物をC:\os32にコピー (sudo不要, 再起動不要)
# ゲストOSは /host 経由で直接アクセス可能
deploy: $(BUILD_OUT)/vmkernel.lz4 programs unicode_bin
	@echo "=== HostDrv Deploy ==="
	$(HOSTDRV_DEPLOY) sync

# deploy-kernel: vmkernel.lz4 と全ビルド成果物をNHDのext2に配置
#   (NP21/W再起動が必要)。HostDrv を先に同期するので、HostDrv 側が古いまま
#   ext2 を上書きして中身を失う事故を防げる。
#   ブートローダー自体を変更した場合は deploy-boot を別途実行すること。
deploy-kernel: $(BUILD_OUT)/vmkernel.lz4
	@echo "=== Sync to HostDrv before NHD deploy ==="
	$(HOSTDRV_DEPLOY) sync
	$(NHD_DEPLOY) sync-from-hostdrv
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
	$(NHD_DEPLOY) deploy

# dp-<name>: 個別プログラムのビルド → シリアル経由でのホットデプロイ(再起動不要)
# dp-<path>: 個別プログラムのホットデプロイ。パスはリポジトリルートから
#   (例: make dp-userland/cmds/wc, make dp-game/app/game)
dp-%: %.bin
	@echo "=== Hot Deploy (Serial Push): $*.bin ==="
	$(NHD_DEPLOY) push $*.bin --resolve

# nhd-mount: NHDのext2パーティションをマウント
nhd-mount:
	$(NHD_DEPLOY) mount

# nhd-umount: NHDのext2パーティションをアンマウント
nhd-umount:
	$(NHD_DEPLOY) umount

# nhd-init: 初回セットアップ (Windows側NHDコピー + フォーマット + マウント)
nhd-init:
	$(NHD_DEPLOY) init

.PHONY: deploy deploy-kernel deploy-boot deploy-nhd nhd-mount nhd-umount nhd-init
