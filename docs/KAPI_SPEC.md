# KernelAPI v17 仕様書

外部プログラム (OS32X) がカーネル機能を利用するためのAPIテーブル仕様。

---

## §1 概要

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| KAPIテーブルアドレス | 0x3F0000 |
| KAPIマジック | 0x4B415049 ('KAPI') |
| プログラムロード先 | 0x400000 |
| 最大プログラムサイズ | 1MB |
| プログラム専用ヒープ | 0x500000〜 (最大1MB、デフォルト256KB) |
| 子プログラム領域 | 0x600000〜 (最大1MB) |
| プログラム専用スタック | 0x4FF000 (下向き64KB) |
| 現在のバージョン | **17** |
| 合計関数ポインタ数 | **97** |

---

## §2 呼び出し規約

| 対象 | コンパイルフラグ | 規約 |
|------|--------|------|
| カーネル本体 | `gcc -m32` | System V i386 ABI (スタック渡し) |
| KernelAPIラッパー | `__attribute__((cdecl))` または通常 | cdecl/System V |
| 外部プログラム | `gcc -m32 -ffreestanding` | System V i386 ABI |

外部プログラムの `main` は `void main(int argc, char **argv, KernelAPI *api)` のシグネチャを持ちます。crt0 が argc/argv と共に api ポインタを渡します。

---

## §3 ビルド手順

```bash
# 一括ビルド (Makefile利用)
make programs
```
外部プログラムは `programs/` 以下に `.c` を置き、`make programs` を実行することで、`crt0.asm` や `libos32` (newlib-nanoラッパー) とともにリンクされ、`mkos32x.py` によってヘッダが付与された `.bin` が生成されます。

---

## §4 KernelAPI 構造体レイアウト

### ヘッダ

| Offset | フィールド | 説明 |
|--------|-----------|------|
| 0x00 | magic | 0x4B415049 ("KAPI") |
| 0x04 | version | APIバージョン (現在: 17) |

### API関数 (自動生成)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x08 | gfx_init | `void(void)` |
| 0x0C | gfx_shutdown | `void(void)` |
| 0x10 | gfx_present | `void(void)` |
| 0x14 | gfx_clear | `void(u8 color)` |
| 0x18 | gfx_pixel | `void(int x, int y, u8 color)` |
| 0x1C | gfx_hline | `void(int x, int y, int w, u8 color)` |
| 0x20 | gfx_vline | `void(int x, int y, int h, u8 color)` |
| 0x24 | gfx_line | `void(int x0, int y0, int x1, int y1, u8 c)` |
| 0x28 | gfx_rect | `void(int x, int y, int w, int h, u8 color)` |
| 0x2C | gfx_fill_rect | `void(int x, int y, int w, int h, u8 c)` |
| 0x30 | kbd_trygetchar | `int(void)` |
| 0x34 | mem_alloc | `void *(u32 size)` |
| 0x38 | mem_free | `void(void *ptr)` |
| 0x3C | get_tick | `u32(void)` |
| 0x40 | kprintf | `void(u8 attr, const char *fmt, ...)` |
| 0x44 | sys_unlink | `int(const char *path)` |
| 0x48 | sys_rename | `int(const char *oldpath, const char *newpath)` |
| 0x4C | sys_mkdir | `int(const char *path)` |
| 0x50 | sys_ls | `int(const char *path, void *cb, void *ctx)` |
| 0x54 | kmalloc_total | `u32(void)` |
| 0x58 | kmalloc_used | `u32(void)` |
| 0x5C | kmalloc_free | `u32(void)` |
| 0x60 | paging_enabled | `int(void)` |
| 0x64 | rtc_read | `void(void *rtc_time)` |
| 0x68 | tvram_clear | `void(void)` |
| 0x6C | tvram_putchar_at | `void(int x, int y, char ch, u8 attr)` |
| 0x70 | tvram_putkanji_at | `void(int x, int y, u16 jis, u8 attr)` |
| 0x74 | tvram_scroll | `void(void)` |
| 0x78 | kbd_getchar | `int(void)` |
| 0x7C | kbd_getkey | `int(void)` |
| 0x80 | kbd_trygetkey | `int(void)` |
| 0x84 | sys_mount | `int(const char *prefix, const char *dev, const char *fs)` |
| 0x88 | sys_umount | `void(const char *prefix)` |
| 0x8C | sys_is_mounted | `int(const char *prefix)` |
| 0x90 | sys_chdir | `int(const char *path)` |
| 0x94 | sys_getcwd | `const char *(void)` |
| 0x98 | vfs_devname | `const char *(const char *prefix)` |
| 0x9C | vfs_sync | `int(void)` |
| 0xA0 | sys_rmdir | `int(const char *path)` |
| 0xA4 | serial_init | `void(u32 baud)` |
| 0xA8 | serial_puts | `void(const char *s)` |
| 0xAC | serial_getchar | `int(void)` |
| 0xB0 | serial_putchar | `void(u8 ch)` |
| 0xB4 | serial_trygetchar | `int(void)` |
| 0xB8 | serial_is_initialized | `int(void)` |
| 0xBC | exec_run | `int(const char *path)` |
| 0xC0 | dev_count | `int(void)` |
| 0xC4 | dev_get_info | `int(int idx, char *name, int nm, int *type, u32 *sects)` |
| 0xC8 | fm_startup_sound | `void(void)` |
| 0xCC | fm_play_mml | `void(const char *mml)` |
| 0xD0 | np2_detect | `int(void)` |
| 0xD4 | np2_get_version | `void(char *buf, int size)` |
| 0xD8 | np2_get_cpu | `void(char *buf, int size)` |
| 0xDC | np2_get_clock | `void(char *buf, int size)` |
| 0xE0 | np2_check_hostdrv | `int(char *buf, int size)` |
| 0xE4 | ide_init | `void(void)` |
| 0xE8 | ide_drive_present | `int(int drv)` |
| 0xEC | ide_identify | `int(int drv, void *info)` |
| 0xF0 | ide_read_sector | `int(int drv, u32 lba, void *buf)` |
| 0xF4 | path_get_drive | `const char *(void)` |
| 0xF8 | path_get_cwd | `const char *(void)` |
| 0xFC | path_set_drive | `int(const char *d)` |
| 0x100 | path_set_cwd | `void(const char *p)` |
| 0x104 | path_parse | `void(const char *input, void *result)` |
| 0x108 | ext2_format | `int(int drv, u32 sectors)` |
| 0x10C | kcg_init | `void(void)` |
| 0x110 | kcg_set_scale | `void(int s)` |
| 0x114 | kcg_draw_ank | `void(int x, int y, u8 ch, u8 fg, u8 bg)` |
| 0x118 | kcg_draw_kanji | `void(int x, int y, u16 jis, u8 fg, u8 bg)` |
| 0x11C | kcg_draw_utf8 | `int(int x, int y, const char *str, u8 fg, u8 bg)` |
| 0x120 | kcg_draw_sjis | `int(int x, int y, const char *str, u8 fg, u8 bg)` |
| 0x124 | buz_on | `void(void)` |
| 0x128 | buz_off | `void(void)` |
| 0x12C | rshell_set_active | `void(int active)` |
| 0x130 | ide_write_sector | `int(int drv, u32 lba, const void *buf)` |
| 0x134 | ide_write_sectors | `int(int drv, u32 lba, u32 cnt, const void *buf)` |
| 0x138 | sys_reboot | `void(void)` |
| 0x13C | sys_halt | `void(void)` |
| 0x140 | shell_putchar | `void(char ch, u8 attr)` |
| 0x144 | console_get_cursor_x | `int(void)` |
| 0x148 | console_get_cursor_y | `int(void)` |
| 0x14C | console_set_cursor | `void(int x, int y)` |
| 0x150 | sys_open | `int(const char *path, int mode)` |
| 0x154 | sys_close | `void(int fd)` |
| 0x158 | sys_read | `int(int fd, void *buf, u32 size)` |
| 0x15C | sys_write | `int(int fd, const void *buf, u32 size)` |
| 0x160 | sys_lseek | `int(int fd, int offset, int whence)` |
| 0x164 | console_get_size | `void(int *w, int *h)` |
| 0x168 | kbd_get_modifiers | `u32(void)` |
| 0x16C | sys_get_mem_kb | `u32(void)` |
| 0x170 | sys_time | `os_time_t(void)` |
| 0x174 | gfx_hardware_scroll | `void(int lines)` |
| 0x178 | gfx_present_rect | `void(int x, int y, int w, int h)` |
| 0x17C | sys_exit | `void(int status)` |
| 0x180 | sys_isatty | `int(int fd)` |
| 0x184 | sys_stat | `int(const char *path, OS32_Stat *buf)` |
| 0x188 | sys_fstat | `int(int fd, OS32_Stat *buf)` |

---

*KernelAPI Specification — Version 17*
*Last Updated: 2026-04-04*
