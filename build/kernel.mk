# ============================================================================
#  kernel.mk — カーネル + SQLite ビルドルール
# ============================================================================

# === カーネル ASM ソース ===
ASM_KERNEL = kernel/kentry.asm kernel/isr_stub.asm kernel/v86_entry.asm kernel/setjmp.asm lib/kstring_asm.asm lib/sqlite3/sqlite_stack.asm
ASM_KERNEL_OBJ = $(ASM_KERNEL:.asm=.o)

# === カーネル C ソース ===
C_KERNEL = \
    kernel/kernel.c kernel/gdt.c kernel/tss.c kernel/v86.c kernel/v86_mem.c kernel/v86_io.c kernel/v86_pic.c kernel/v86_kbd.c kernel/v86_bios.c kernel/boot_splash.c kernel/idt.c kernel/isr_handlers.c kernel/cpu_calibrate.c \
    kernel/paging.c kernel/pgalloc.c kernel/shm.c kernel/kmalloc.c kernel/console.c kernel/sys.c kernel/kselftest.c \
    kernel/ime.c kernel/ime_romkana.c kernel/ime_dict.c kernel/ime_render_tvram.c kernel/snd_engine.c \
    drivers/kbd.c drivers/serial.c drivers/fm.c \
    drivers/fdc.c drivers/disk.c drivers/ide.c drivers/atapi.c drivers/rtc.c drivers/dev.c drivers/kcg.c drivers/np2sysp.c drivers/loop_dev.c \
    drivers/mouse.c drivers/mouse_bus.c drivers/mouse_seamless.c \
    gfx/gfx_core.c gfx/gfx_vram.c gfx/gfx_scroll.c gfx/palette.c \
    fs/fatfs/ff.c fs/fatfs/diskio.c fs/fatfs_vfs.c \
    fs/ext2_super.c fs/ext2_inode.c fs/ext2_dir.c fs/ext2_file.c fs/ext2_fmt.c fs/ext2_vfs.c fs/vfs.c fs/vfs_fd.c fs/fd_redirect.c fs/pipe_buffer.c fs/iso9660.c fs/hostdrvfs.c \
    exec/exec.c exec/exec_heap.c \
    kapi/kapi_generated.c kapi/kapi_db.c kapi/kapi_sys.c \
    lib/path.c lib/utf8.c lib/kprintf.c lib/os_time.c lib/kstring.c lib/kutf16.c lib/kmath.c

C_KERNEL_OBJ = $(C_KERNEL:.c=.o)

# === SQLite関連 (カーネル拡張域 0x200000 に配置) ===
C_SQLITE = lib/sqlite3/sqlite3.c lib/sqlite3/os32_sqlite_vfs.c lib/sqlite3/os32_sqlite_test.c
C_SQLITE_OBJ = $(C_SQLITE:.c=.o)

# === モジュール別コンパイルルール ===

# kernel/ モジュール
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS_BASE) $(INC_KERNEL) -c $< -o $@

# drivers/ モジュール
# mouse.c は idt.h (IRQ制御) を参照するため INC_KERNEL でビルド
drivers/mouse.o: drivers/mouse.c
	$(CC) $(CFLAGS_BASE) $(INC_KERNEL) -c $< -o $@

# loop_dev.c は vfs.h / kprintf.h (fs/, lib/) を参照するため INC_KERNEL でビルド
drivers/loop_dev.o: drivers/loop_dev.c
	$(CC) $(CFLAGS_BASE) $(INC_KERNEL) -c $< -o $@

drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS_BASE) $(INC_DRIVERS) -c $< -o $@

# gfx/ モジュール
gfx/%.o: gfx/%.c
	$(CC) $(CFLAGS_BASE) $(INC_GFX) -c $< -o $@

# fs/ モジュール
fs/%.o: fs/%.c
	$(CC) $(CFLAGS_BASE) $(INC_FS) -c $< -o $@

# fs/fatfs/ モジュール (FatFs公式ソース — string.hスタブ付き)
fs/fatfs/ff.o: fs/fatfs/ff.c fs/fatfs/ffconf.h fs/fatfs/string.h
	$(CC) $(CFLAGS_BASE) $(INC_FATFS) -c $< -o $@

fs/fatfs/diskio.o: fs/fatfs/diskio.c fs/fatfs/ff.h fs/fatfs/diskio.h
	$(CC) $(CFLAGS_BASE) $(INC_FATFS) -c $< -o $@

# exec/ モジュール
exec/%.o: exec/%.c kapi/kapi_generated.c
	$(CC) $(CFLAGS_BASE) $(INC_EXEC) -c $< -o $@

# kapi/ モジュール
kapi/kapi_generated.c: sdk/kapi.json sdk/gen_kapi.py
	python3 sdk/gen_kapi.py

# kapi_sys.o は __DATE__/__TIME__ を含むため毎回再コンパイル
kapi/kapi_sys.o: kapi/kapi_sys.c .FORCE
	$(CC) $(CFLAGS_BASE) $(INC_KAPI) -c $< -o $@

kapi/%.o: kapi/%.c kapi/kapi_generated.c
	$(CC) $(CFLAGS_BASE) $(INC_KAPI) -c $< -o $@

.FORCE:

# lib/ モジュール (汎用ライブラリ: カーネル依存なし)
lib/%.o: lib/%.c
	$(CC) $(CFLAGS_BASE) $(INC_LIB) -c $< -o $@

# === Rust LZ4デコーダ (lib/os32_lz4/) ===
# C版 (lib/lz4.c) をRust実装に置き換え。境界チェック付きの安全な展開を保証。
RUST_LZ4_DIR = lib/os32_lz4
RUST_LZ4_LIB = $(RUST_LZ4_DIR)/target/i686-os32-none/release/libos32_lz4.a

$(RUST_LZ4_LIB): $(RUST_LZ4_DIR)/src/lib.rs $(RUST_LZ4_DIR)/Cargo.toml
	cd $(RUST_LZ4_DIR) && cargo build --release

# SQLite (カーネル拡張域配置 — -Os必須)
lib/sqlite3/sqlite3.o: lib/sqlite3/sqlite3.c lib/sqlite3/os32_sqlite_config.h
	$(CC) $(CFLAGS_SQLITE) -include lib/sqlite3/os32_sqlite_config.h $(INC_SQLITE) -c $< -o $@

lib/sqlite3/os32_sqlite_vfs.o: lib/sqlite3/os32_sqlite_vfs.c lib/sqlite3/os32_sqlite_vfs.h lib/sqlite3/os32_sqlite_config.h
	$(CC) $(CFLAGS_SQLITE) -include lib/sqlite3/os32_sqlite_config.h $(INC_SQLITE) -c $< -o $@

lib/sqlite3/os32_sqlite_test.o: lib/sqlite3/os32_sqlite_test.c lib/sqlite3/os32_sqlite_vfs.h lib/sqlite3/os32_sqlite_config.h
	$(CC) -std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -mno-red-zone -O0 -fcommon -Wno-long-long -w $(DEPFLAGS) -include lib/sqlite3/os32_sqlite_config.h $(INC_SQLITE) -c $< -o $@

# === カーネルリンク ===
$(BUILD_OUT)/kernel.elf: $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) $(C_SQLITE_OBJ) $(RUST_LZ4_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) $(C_SQLITE_OBJ) $(RUST_LZ4_LIB) -lgcc

$(BUILD_OUT)/kernel.bin: $(BUILD_OUT)/kernel.elf
	$(OBJCOPY) -O binary \
		--remove-section=.sqlite_text \
		--remove-section=.sqlite_rodata \
		--remove-section=.sqlite_data \
		--remove-section=.sqlite_bss \
		$< $@

# SQLite 拡張域バイナリ (0x200000 にロードされる)
$(BUILD_OUT)/sqlite.bin: $(BUILD_OUT)/kernel.elf
	$(OBJCOPY) -O binary \
		--only-section=.sqlite_text \
		--only-section=.sqlite_rodata \
		--only-section=.sqlite_data \
		$< $@

# === VK32 圧縮カーネルイメージ ===
$(BUILD_OUT)/vmkernel.lz4: $(BUILD_OUT)/kernel.bin $(BUILD_OUT)/sqlite.bin
	python3 tools/mkvmkernel.py \
		--kernel $(BUILD_OUT)/kernel.bin --kernel-addr 0x100000 \
		--sqlite $(BUILD_OUT)/sqlite.bin --sqlite-addr 0x200000 \
		-o $(BUILD_OUT)/vmkernel.lz4

# === カーネル単独ターゲット ===
kernel: $(BUILD_OUT)/kernel.bin $(BUILD_OUT)/sqlite.bin $(BUILD_OUT)/vmkernel.lz4

# === カーネルクリーン ===
clean-kernel:
	rm -f $(ASM_KERNEL_OBJ) $(C_KERNEL_OBJ) $(C_SQLITE_OBJ) $(BUILD_OUT)/kernel.elf $(BUILD_OUT)/kernel.bin $(BUILD_OUT)/sqlite.bin $(BUILD_OUT)/vmkernel.lz4 $(BUILD_OUT)/kernel.map
	cd $(RUST_LZ4_DIR) && cargo clean 2>/dev/null || true

.PHONY: kernel clean-kernel
