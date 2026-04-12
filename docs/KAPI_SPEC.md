# KernelAPI v24 仕様書

外部プログラム (OS32X) がカーネル機能を利用するためのAPIテーブル仕様。

---

## §1 概要

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| KAPIテーブルアドレス | 0x189000 |
| KAPIマジック | 0x4B415049 ('KAPI') |
| プログラムロード先 | 0x400000 |
| 最大プログラムサイズ | 1MB |
| プログラム専用ヒープ | 動的配置 (sbrk_heap_limit, exec_heap 管理下) |
| プログラム専用スタック | 動的配置 (メモリ終端付近、下向き展開) |
| 現在のバージョン | **24** |
| 合計エントリ数 | **108** (データフィールド1 + 関数ポインタ107) |

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
| 0x04 | version | APIバージョン (現在: 22) |

### データフィールド

| Offset | フィールド | 型 | 説明 |
|--------|-----------|------|------|
| 0x08 | sbrk_heap_limit | `u32` | newlib _sbrk用ヒープ上限アドレス (exec_runでセットされる) |

### API関数 (自動生成)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x0C | gfx_init | `void(void)` |
| 0x10 | gfx_shutdown | `void(void)` |
| 0x14 | gfx_present | `void(void)` |
| 0x18 | kbd_trygetchar | `int(void)` |
| 0x1C | mem_alloc | `void *(u32 size)` |
| 0x20 | mem_free | `void(void *ptr)` |
| 0x24 | get_tick | `u32(void)` |
| 0x28 | kprintf | `void(u8 attr, const char *fmt, ...)` |
| 0x2C | sys_unlink | `int(const char *path)` |
| 0x30 | sys_rename | `int(const char *oldpath, const char *newpath)` |
| 0x34 | sys_mkdir | `int(const char *path)` |
| 0x38 | sys_ls | `int(const char *path, void *cb, void *ctx)` |
| 0x3C | kmalloc_total | `u32(void)` |
| 0x40 | kmalloc_used | `u32(void)` |
| 0x44 | kmalloc_free | `u32(void)` |
| 0x48 | paging_enabled | `int(void)` |
| 0x4C | rtc_read | `void(void *rtc_time)` |
| 0x50 | tvram_clear | `void(void)` |
| 0x54 | tvram_putchar_at | `void(int x, int y, char ch, u8 attr)` |
| 0x58 | tvram_putkanji_at | `void(int x, int y, u16 jis, u8 attr)` |
| 0x5C | tvram_scroll | `void(void)` |
| 0x60 | kbd_getchar | `int(void)` |
| 0x64 | kbd_getkey | `int(void)` |
| 0x68 | kbd_trygetkey | `int(void)` |
| 0x6C | sys_mount | `int(const char *prefix, const char *dev, const char *fs)` |
| 0x70 | sys_umount | `void(const char *prefix)` |
| 0x74 | sys_is_mounted | `int(const char *prefix)` |
| 0x78 | sys_chdir | `int(const char *path)` |
| 0x7C | sys_getcwd | `const char *(void)` |
| 0x80 | vfs_devname | `const char *(const char *prefix)` |
| 0x84 | vfs_sync | `int(void)` |
| 0x88 | sys_rmdir | `int(const char *path)` |
| 0x8C | serial_init | `void(u32 baud)` |
| 0x90 | serial_puts | `void(const char *s)` |
| 0x94 | serial_getchar | `int(void)` |
| 0x98 | serial_putchar | `void(u8 ch)` |
| 0x9C | serial_trygetchar | `int(void)` |
| 0xA0 | serial_is_initialized | `int(void)` |
| 0xA4 | exec_run | `int(const char *path)` |
| 0xA8 | dev_count | `int(void)` |
| 0xAC | dev_get_info | `int(int idx, char *name, int nm, int *type, u32 *sects)` |
| 0xB0 | fm_startup_sound | `void(void)` |
| 0xB4 | fm_play_mml | `void(const char *mml)` |
| 0xB8 | np2_detect | `int(void)` |
| 0xBC | np2_get_version | `void(char *buf, int size)` |
| 0xC0 | np2_get_cpu | `void(char *buf, int size)` |
| 0xC4 | np2_get_clock | `void(char *buf, int size)` |
| 0xC8 | np2_check_hostdrv | `int(char *buf, int size)` |
| 0xCC | ide_init | `void(void)` |
| 0xD0 | ide_drive_present | `int(int drv)` |
| 0xD4 | ide_identify | `int(int drv, void *info)` |
| 0xD8 | ide_read_sector | `int(int drv, u32 lba, void *buf)` |
| 0xDC | path_get_drive | `const char *(void)` |
| 0xE0 | path_get_cwd | `const char *(void)` |
| 0xE4 | path_set_drive | `int(const char *d)` |
| 0xE8 | path_set_cwd | `void(const char *p)` |
| 0xEC | path_parse | `void(const char *input, void *result)` |
| 0xF0 | ext2_format | `int(int drv, u32 sectors)` |
| 0xF4 | kcg_init | `void(void)` |
| 0xF8 | kcg_set_scale | `void(int s)` |
| 0xFC | buz_on | `void(void)` |
| 0x100 | buz_off | `void(void)` |
| 0x104 | rshell_set_active | `void(int active)` |
| 0x108 | ide_write_sector | `int(int drv, u32 lba, const void *buf)` |
| 0x10C | ide_write_sectors | `int(int drv, u32 lba, u32 cnt, const void *buf)` |
| 0x110 | sys_reboot | `void(void)` |
| 0x114 | sys_halt | `void(void)` |
| 0x118 | shell_putchar | `void(char ch, u8 attr)` |
| 0x11C | console_get_cursor_x | `int(void)` |
| 0x120 | console_get_cursor_y | `int(void)` |
| 0x124 | console_set_cursor | `void(int x, int y)` |
| 0x128 | sys_open | `int(const char *path, int mode)` |
| 0x12C | sys_close | `void(int fd)` |
| 0x130 | sys_read | `int(int fd, void *buf, u32 size)` |
| 0x134 | sys_write | `int(int fd, const void *buf, u32 size)` |
| 0x138 | sys_lseek | `int(int fd, int offset, int whence)` |
| 0x13C | console_get_size | `void(int *w, int *h)` |
| 0x140 | kbd_get_modifiers | `u32(void)` |
| 0x144 | sys_get_mem_kb | `u32(void)` |
| 0x148 | sys_time | `os_time_t(void)` |
| 0x14C | gfx_hardware_scroll | `void(int lines)` |
| 0x150 | gfx_present_rect | `void(int x, int y, int w, int h)` |
| 0x154 | sys_exit | `void(int status)` |
| 0x158 | sys_isatty | `int(int fd)` |
| 0x15C | sys_stat | `int(const char *path, OS32_Stat *buf)` |
| 0x160 | sys_fstat | `int(int fd, OS32_Stat *buf)` |
| 0x164 | gfx_set_palette | `void(int idx, u8 r, u8 g, u8 b)` |
| 0x168 | gfx_get_palette | `void(int idx, u8 *r, u8 *g, u8 *b)` |
| 0x16C | sys_memcpy | `void*(void *dst, const void *src, u32 n)` |
| 0x170 | sys_memset | `void*(void *dst, int val, u32 n)` |
| 0x174 | gfx_get_framebuffer | `void(void *fb)` |
| 0x178 | gfx_add_dirty_rect | `void(int x, int y, int w, int h)` |
| 0x17C | gfx_present_dirty | `void(void)` |
| 0x180 | gfx_present_raster | `void(void *table)` |
| 0x184 | kcg_read_ank | `void(u8 ch, u8 *buf)` |
| 0x188 | kcg_read_kanji | `void(u16 jis_code, u8 *buf)` |

### §4-1 グラフィックスAPI に関する補足

v22以降、基本的な描画プリミティブ (`gfx_clear`, `gfx_pixel`, `gfx_hline`, `gfx_vline`, `gfx_line`, `gfx_rect`, `gfx_fill_rect`) は KernelAPI から**廃止**されました。

外部プログラムでグラフィックス描画を行う場合は、以下の２つの方式から選択します:

1. **libos32gfx ライブラリ** (推奨): `programs/libos32gfx/` で提供されるスタティックリンクライブラリ。サーフェス、スプライト、描画プリミティブ、ダーティ矩形管理、フォントレンダリングなど高レベルな描画機能を提供します。
2. **フレームバッファ直接操作**: `gfx_get_framebuffer()` で取得した `GFX_Framebuffer` 構造体を介して、4プレーンのバックバッファに直接書き込み、`gfx_add_dirty_rect()` + `gfx_present_dirty()` でVRAMに転送します。

### §4-2 ラスタパレット (gfx_present_raster)

v24で追加。VSYNC後のアクティブ表示期間中に、走査線ごとにパレットレジスタを書き換えることで、16色パレットの制約を超えた擬似多色表示を実現します。

- **引数**: `GFX_RasterPalTable *table` — ラスタパレットテーブルへのポインタ
- **構造体**: `GFX_RasterPalEntry` (line, pal_idx, r, g, b) × 最大200エントリ
- **動作**: dirty rectがあればVRAM転送も行い、なければパレット書き換えのみ
- **libos32gfx ラッパー**: `gfx_raster_clear()`, `gfx_raster_add()`, `gfx_present_raster_only()`, `gfx_present_with_raster()`

---

*KernelAPI Specification — Version 24*
*Last Updated: 2026-04-12*
