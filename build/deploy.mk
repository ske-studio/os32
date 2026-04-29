# ============================================================================
#  deploy.mk — デプロイターゲット
# ============================================================================

# deploy: HostDrv方式 — ビルド成果物をC:\os32にコピー (sudo不要, 再起動不要)
# ゲストOSは /host 経由で直接アクセス可能
deploy: kernel.bin programs unicode_bin
	@echo "=== HostDrv Deploy ==="
	$(HOSTDRV_DEPLOY) sync

# deploy-kernel: カーネル+SQLiteをNHDブート領域に書き込み + HostDrvからext2同期 (NP21/W再起動が必要)
# C:\os32 (HostDrv) の内容をNHDのext2パーティションにも反映する
deploy-kernel: kernel.bin sqlite.bin
	$(NHD_DEPLOY) write-kernel kernel.bin boot/loader_hdd.bin sqlite.bin
	$(NHD_DEPLOY) sync-from-hostdrv
	$(NHD_DEPLOY) deploy

# deploy-nhd: 旧方式NHDフルデプロイ (カーネル+全ファイル)
deploy-nhd: kernel.bin programs unicode_bin
	@echo "=== NHD Deploy (using deploy.yaml) ==="
	$(NHD_DEPLOY) sync
	$(NHD_DEPLOY) deploy

# dp-<name>: 個別プログラムのビルド → シリアル経由でのホットデプロイ(再起動不要)
# NHDイメージへの書き込みを行わず、実行中のOS32へファイルを転送する
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
