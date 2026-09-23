# ============================================================================
#  image.mk — D88/ISO イメージ生成
# ============================================================================

# FDD最小ブートイメージ (images/os32_boot.d88)
# HDDインストール用ブートFD。必須コマンドのみ含む。
# cfg は票 S3 (リカバリ後に FDD 自身のマスタを cfg status で確認する用途。HDD の DB には使えない)
FDD_MIN_CMDS = more less grep find sort head tail wc tee touch hexdump sleep diff du cal man sndctl cfg
# FDD イメージに入れるファイル一覧。**2hd と 1.44MB で 1 つを使う** —
# 写すと片方だけ更新されて中身がずれる。$(1) = LOADER.BIN のもと。
define FDD_IMAGE_ARGS
args="--tree"; \
	args="$$args /LOADER.BIN=$(1)"; \
	args="$$args /VMKRNL.LZ4=$(BUILD_OUT)/vmkernel.lz4"; \
	args="$$args /sys/shell.bin=userland/shell.bin"; \
	args="$$args /sys/unicode.bin=$(BUILD_OUT)/unicode.bin"; \
	args="$$args /sys/boot_hdd.bin=boot/boot_hdd.bin"; \
	args="$$args /sys/loader_h.bin=boot/loader_hdd.bin"; \
	for cmd in $$(echo $(FDD_MIN_CMDS)); do \
		if [ -f "userland/cmds/$$cmd.bin" ]; then \
			args="$$args /bin/$$cmd.bin=userland/cmds/$$cmd.bin"; \
		elif [ -f "userland/system/$$cmd.bin" ]; then \
			args="$$args /bin/$$cmd.bin=userland/system/$$cmd.bin"; \
		fi; \
	done; \
	args="$$args /sbin/install.bin=userland/system/install.bin"; \
	args="$$args /bin/timetest.bin=userland/tests/time_test.bin"; \
	args="$$args /sbin/cdinst.bin=userland/system/cdinst.bin"; \
	if [ -f assets/profile_fdd ]; then args="$$args /etc/profile=assets/profile_fdd"; fi; \
	args="$$args /etc/settings.db=$(BUILD_OUT)/settings.db"
endef

images/os32_boot.d88: boot $(BUILD_OUT)/vmkernel.lz4 programs unicode_bin $(BUILD_OUT)/settings.db
	@mkdir -p images
	@echo "=== Building OS32 minimal FDD image (images/os32_boot.d88) ==="
	@$(call FDD_IMAGE_ARGS,boot/loader_fat_new.bin); \
	python3 tools/mkfat12.py -o images/os32_boot.img -b boot/boot_fat.bin -d images/os32_boot.d88 $$args
	@echo "Copying os32_boot.d88 to NP21/W directory..."
	@cp images/os32_boot.d88 '$(NP21W_DIR)/os32_boot.d88' 2>/dev/null || echo "Warning: Failed to copy os32_boot.d88 to np21w directory."

# --- 1.44MB 版 (生イメージ) ---
# **D88 にしない。** 1.44MB の D88 は fd_type=0x21 かつ全セクタの rpm_flg=1 が
# 要り、tools/mkd88.py はまだ書けない。NP21/W は 1,474,560 バイトちょうどの
# 生イメージをサイズで 1.44MB と判定する (src/diskimage/fd/fdd_xdf.c の表)。
images/os32_boot144.img: boot $(BUILD_OUT)/vmkernel.lz4 programs unicode_bin $(BUILD_OUT)/settings.db
	@mkdir -p images
	@echo "=== Building OS32 1.44MB FDD image (images/os32_boot144.img) ==="
	@$(call FDD_IMAGE_ARGS,boot/loader_fat144.bin); \
	python3 tools/mkfat12.py -g 144 -o images/os32_boot144.img -b boot/boot_fat144.bin $$args
	@SIZE=$$(stat -c%s images/os32_boot144.img); \
	if [ "$$SIZE" != "1474560" ]; then \
		echo "ERROR: $$SIZE バイト。NP21/W は 1474560 ちょうどでないと 1.44MB と見ない"; \
		exit 1; \
	fi
	@cp images/os32_boot144.img '$(NP21W_DIR)/os32_boot144.img' 2>/dev/null || echo "Warning: Failed to copy os32_boot144.img to np21w directory."

fd144: images/os32_boot144.img
.PHONY: fd144

# パッケージ / ISO生成
# mkpkg は登録ファイルの欠損をエラーにするので、core / userland のパッケージ定義が
# 要求する入力をすべて依存に結ぶ (clean 後の単独 `make iso` や `make -j` でも
# 欠損で落ちないように)。assets/fep.db は userland 層の NORMAL が要求する。
packages: programs boot $(BUILD_OUT)/vmkernel.lz4 unicode_bin \
          $(BUILD_OUT)/settings.db assets/fep.db
	python3 tools/mkpkg.py --defs build/core_packages.yaml \
	                     --defs userland/package_defs.yaml \
	                     --output packages/ --base .

iso: packages
	@mkdir -p images
	genisoimage -o images/os32_install.iso -V "OS32_INSTALL" -input-charset utf-8 -R packages/

# イメージクリーン
clean-images:
	rm -f packages/*.PKG images/os32_install.iso os32_boot.img os32_boot.d88
	rm -rf images

.PHONY: packages iso clean-images
