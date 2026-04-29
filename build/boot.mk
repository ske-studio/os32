# ============================================================================
#  boot.mk — ブートローダーのビルドルール
# ============================================================================

# === スタンドアロン ASM (flat binary) ===
ASM_STANDALONE = boot/boot_fat.asm boot/loader_fat.asm boot/loader_fat_new.asm boot/boot_hdd.asm boot/loader_hdd.asm
BIN_STANDALONE = $(ASM_STANDALONE:.asm=.bin)

# === 新HDDローダー (ASM + C リンク) ===
CFLAGS_BOOT = -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie \
              -fno-stack-protector -nostdlib -mno-red-zone -Os -Wall -fcommon \
              -Iboot

BOOT_C_SRC = boot/boot_main.c boot/ext2_mini.c boot/lz4_mini.c
BOOT_C_OBJ = $(BOOT_C_SRC:.c=.o)
BOOT_ASM_OBJ = boot/loader_hdd.o
BOOT_ALL_OBJ = $(BOOT_ASM_OBJ) $(BOOT_C_OBJ)

boot/loader_hdd.o: boot/loader_hdd.asm
	$(AS) -f elf32 -o $@ $<

boot/boot_main.o: boot/boot_main.c boot/boot_defs.h
	$(CC) $(CFLAGS_BOOT) -c -o $@ $<

boot/ext2_mini.o: boot/ext2_mini.c boot/boot_defs.h
	$(CC) $(CFLAGS_BOOT) -c -o $@ $<

boot/lz4_mini.o: boot/lz4_mini.c boot/boot_defs.h
	$(CC) $(CFLAGS_BOOT) -c -o $@ $<

boot/loader_hdd.elf: $(BOOT_ALL_OBJ)
	$(LD) -m elf_i386 -T boot/loader.ld -o $@ $^ \
		-L$(shell $(CC) -print-libgcc-file-name | xargs dirname) -lgcc

boot/loader_hdd.bin: boot/loader_hdd.elf
	$(OBJCOPY) -O binary $< $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -gt 8192 ]; then \
		echo "ERROR: loader_hdd.bin is $$SIZE bytes (max 8192)"; \
		exit 1; \
	else \
		echo "loader_hdd.bin: $$SIZE / 8192 bytes"; \
	fi

# === 統合ターゲット ===
boot: $(BIN_STANDALONE) boot/loader_hdd.bin

# === クリーン ===
clean-boot:
	rm -f $(BIN_STANDALONE) $(BOOT_ALL_OBJ) boot/loader_hdd.elf boot/loader_hdd.bin

# === デバッグ版HDDローダー (ext2読み出しステップ確認用) ===
BOOT_DBG_C_SRC = boot/boot_debug.c
BOOT_DBG_C_OBJ = $(BOOT_DBG_C_SRC:.c=.o)
BOOT_DBG_ALL_OBJ = $(BOOT_ASM_OBJ) $(BOOT_DBG_C_OBJ)

boot/boot_debug.o: boot/boot_debug.c boot/boot_defs.h
	$(CC) $(CFLAGS_BOOT) -c -o $@ $<

boot/loader_hdd_debug.elf: $(BOOT_DBG_ALL_OBJ)
	$(LD) -m elf_i386 -T boot/loader.ld -o $@ $^ \
		-L$(shell $(CC) -print-libgcc-file-name | xargs dirname) -lgcc

boot/loader_hdd_debug.bin: boot/loader_hdd_debug.elf
	$(OBJCOPY) -O binary $< $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -gt 8192 ]; then \
		echo "ERROR: loader_hdd_debug.bin is $$SIZE bytes (max 8192)"; \
		exit 1; \
	else \
		echo "loader_hdd_debug.bin: $$SIZE / 8192 bytes"; \
	fi

boot-debug: boot/loader_hdd_debug.bin

clean-boot-debug:
	rm -f boot/boot_debug.o boot/loader_hdd_debug.elf boot/loader_hdd_debug.bin

.PHONY: boot clean-boot boot-debug clean-boot-debug
