# ============================================================================
#  image.mk — D88/ISO イメージ生成
# ============================================================================

# FDD最小ブートイメージ (images/os32_boot.d88)
# HDDインストール用ブートFD。必須コマンドのみ含む。
FDD_MIN_CMDS = more less grep find sort head tail wc tee touch hexdump sleep diff du cal man sndctl
images/os32_boot.d88: boot vmkernel.lz4 programs unicode_bin
	@mkdir -p images
	@echo "=== Building OS32 minimal FDD image (images/os32_boot.d88) ==="
	@args="--tree"; \
	args="$$args /LOADER.BIN=boot/loader_fat_new.bin"; \
	args="$$args /VMKRNL.LZ4=vmkernel.lz4"; \
	args="$$args /sys/shell.bin=programs/shell.bin"; \
	args="$$args /sys/unicode.bin=unicode.bin"; \
	args="$$args /sys/boot_hdd.bin=boot/boot_hdd.bin"; \
	args="$$args /sys/loader_h.bin=boot/loader_hdd.bin"; \
	for cmd in $$(echo $(FDD_MIN_CMDS)); do \
		if [ -f "programs/cmds/$$cmd.bin" ]; then \
			args="$$args /bin/$$cmd.bin=programs/cmds/$$cmd.bin"; \
		elif [ -f "programs/system/$$cmd.bin" ]; then \
			args="$$args /bin/$$cmd.bin=programs/system/$$cmd.bin"; \
		fi; \
	done; \
	args="$$args /bin/edit.bin=programs/apps/edit.bin"; \
	args="$$args /sbin/install.bin=programs/system/install.bin"; \
	args="$$args /sbin/cdinst.bin=programs/system/cdinst.bin"; \
	if [ -f assets/profile_fdd ]; then args="$$args /etc/profile=assets/profile_fdd"; fi; \
	python3 tools/mkfat12.py -o images/os32_boot.img -b boot/boot_fat.bin -d images/os32_boot.d88 $$args
	@echo "Copying os32_boot.d88 to NP21/W directory..."
	@cp images/os32_boot.d88 '$(NP21W_DIR)/os32_boot.d88' 2>/dev/null || echo "Warning: Failed to copy os32_boot.d88 to np21w directory."

# パッケージ / ISO生成
packages: programs
	python3 tools/mkpkg.py --defs tools/package_defs.yaml --output packages/ --base .

iso: packages
	@mkdir -p images
	genisoimage -o images/os32_install.iso -V "OS32_INSTALL" -input-charset utf-8 -R packages/

# イメージクリーン
clean-images:
	rm -f packages/*.PKG images/os32_install.iso os32_boot.img os32_boot.d88
	rm -rf images

.PHONY: packages iso clean-images
