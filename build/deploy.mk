# ============================================================================
#  deploy.mk — デプロイターゲット
# ============================================================================

# deploy: HostDrv方式 — ビルド成果物をC:\os32にコピー (sudo不要, 再起動不要)
# ゲストOSは /host 経由で直接アクセス可能
deploy: vmkernel.lz4 programs unicode_bin
	@echo "=== HostDrv Deploy ==="
	$(HOSTDRV_DEPLOY) sync

# deploy-kernel: ローダーをNHDブート領域に書き込み + vmkernel.lz4をext2に配置
#   + HostDrvからext2同期 (NP21/W再起動が必要)
deploy-kernel: vmkernel.lz4
	$(NHD_DEPLOY) write-boot boot/loader_hdd_new.bin
	$(NHD_DEPLOY) sync-from-hostdrv
	$(NHD_DEPLOY) deploy

# deploy-nhd: NHDフルデプロイ (ローダー+全ファイル)
deploy-nhd: vmkernel.lz4 programs unicode_bin
	@echo "=== NHD Deploy (using deploy.yaml) ==="
	$(NHD_DEPLOY) sync
	$(NHD_DEPLOY) deploy

# dp-<name>: 個別プログラムのビルド → シリアル経由でのホットデプロイ(再起動不要)
dp-%: programs/%.bin
	@echo "=== Hot Deploy (Serial Push): $*.bin ==="
	$(NHD_DEPLOY) push programs/$*.bin --resolve

# nhd-mount: NHDのext2パーティションをマウント
nhd-mount:
	$(NHD_DEPLOY) mount

# nhd-umount: NHDのext2パーティションをアンマウント
nhd-umount:
	$(NHD_DEPLOY) umount

# nhd-init: 初回セットアップ (Windows側NHDコピー + フォーマット + マウント)
nhd-init:
	$(NHD_DEPLOY) init

.PHONY: deploy deploy-kernel deploy-nhd nhd-mount nhd-umount nhd-init
