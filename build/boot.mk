# ============================================================================
#  boot.mk — ブートローダーのアセンブルルール
# ============================================================================

ASM_STANDALONE = boot/boot_fat.asm boot/loader_fat.asm boot/boot_hdd.asm boot/loader_hdd.asm
BIN_STANDALONE = $(ASM_STANDALONE:.asm=.bin)

boot: $(BIN_STANDALONE)

.PHONY: boot
