# KernelAPI v38 — 関数テーブル

[← KAPI_SPEC.md に戻る](KAPI_SPEC.md)

自動生成元: `tools/kapi.json` (version 38)

---

## API関数 (162エントリ)

### グラフィックス

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x008 | gfx_init | `void(void)` |
| 0x00C | gfx_init_200 | `void(void)` |
| 0x010 | gfx_shutdown | `void(void)` |
| 0x014 | gfx_present | `void(void)` |
| 0x148 | gfx_hardware_scroll | `void(int lines)` |
| 0x14C | gfx_present_rect | `void(int x, int y, int w, int h)` |
| 0x160 | gfx_set_palette | `void(int idx, u8 r, u8 g, u8 b)` |
| 0x164 | gfx_get_palette | `void(int idx, u8 *r, u8 *g, u8 *b)` |
| 0x168 | gfx_get_framebuffer | `void(void *fb)` |
| 0x16C | gfx_add_dirty_rect | `void(int x, int y, int w, int h)` |
| 0x170 | gfx_present_dirty | `void(void)` |
| 0x174 | gfx_present_nosync | `void(void)` |
| 0x178 | gfx_present_raster | `void(void *table)` |

### キーボード・入力

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x018 | kbd_trygetchar | `int(void)` |
| 0x060 | kbd_getchar | `int(void)` |
| 0x064 | kbd_getkey | `int(void)` |
| 0x068 | kbd_trygetkey | `int(void)` |
| 0x13C | kbd_get_modifiers | `u32(void)` |
| 0x1F4 | kbd_is_pressed | `int(int scancode)` |

### メモリ管理

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x01C | mem_alloc | `void *(u32 size)` |
| 0x020 | mem_free | `void(void *ptr)` |
| 0x03C | kmalloc_total | `u32(void)` |
| 0x040 | kmalloc_used | `u32(void)` |
| 0x044 | kmalloc_free | `u32(void)` |
| 0x184 | sys_shm_alloc | `void *(int blocks)` |
| 0x188 | sys_shm_lock | `int(void *ptr)` |
| 0x18C | sys_shm_free | `int(void *ptr)` |

### システム・タイマ

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x024 | get_tick | `u32(void)` |
| 0x028 | kprintf | `void(u8 attr, const char *fmt, ...)` |
| 0x048 | paging_enabled | `int(void)` |
| 0x04C | rtc_read | `void(void *rtc_time)` |
| 0x108 | sys_reboot | `void(void)` |
| 0x10C | sys_halt | `void(void)` |
| 0x140 | sys_get_mem_kb | `u32(void)` |
| 0x144 | sys_time | `os_time_t(void)` |
| 0x150 | sys_exit | `void(int status)` |
| 0x1D4 | paging_is_present | `int(u32 addr)` |
| 0x270 | sys_get_build_info | `void(char *buf, int size)` |

### TVRAM・コンソール

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x050 | tvram_clear | `void(void)` |
| 0x054 | tvram_putchar_at | `void(int x, int y, char ch, u8 attr)` |
| 0x058 | tvram_putkanji_at | `void(int x, int y, u16 jis, u8 attr)` |
| 0x05C | tvram_scroll | `void(void)` |
| 0x110 | shell_putchar | `void(char ch, u8 attr)` |
| 0x114 | shell_print_utf8 | `void(const char *utf8_str, u8 color)` |
| 0x118 | console_get_cursor_x | `int(void)` |
| 0x11C | console_get_cursor_y | `int(void)` |
| 0x120 | console_set_cursor | `void(int x, int y)` |
| 0x138 | console_get_size | `void(int *w, int *h)` |
| 0x21C | tvram_readchar_at | `void(int x, int y, u16 *code, u8 *attr)` |
| 0x220 | tvram_reverse_cell | `int(int x, int y)` |

### ファイルシステム・VFS

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x02C | sys_unlink | `int(const char *path)` |
| 0x030 | sys_rename | `int(const char *oldpath, const char *newpath)` |
| 0x034 | sys_mkdir | `int(const char *path)` |
| 0x038 | sys_ls | `int(const char *path, void *cb, void *ctx)` |
| 0x06C | sys_mount | `int(const char *prefix, const char *dev, const char *fs)` |
| 0x070 | sys_umount | `void(const char *prefix)` |
| 0x074 | sys_is_mounted | `int(const char *prefix)` |
| 0x078 | sys_chdir | `int(const char *path)` |
| 0x07C | sys_getcwd | `const char *(void)` |
| 0x080 | vfs_devname | `const char *(const char *prefix)` |
| 0x084 | vfs_sync | `int(void)` |
| 0x088 | sys_rmdir | `int(const char *path)` |
| 0x0A4 | exec_run | `int(const char *path)` |
| 0x124 | sys_open | `int(const char *path, int mode)` |
| 0x128 | sys_close | `void(int fd)` |
| 0x12C | sys_read | `int(int fd, void *buf, u32 size)` |
| 0x130 | sys_write | `int(int fd, const void *buf, u32 size)` |
| 0x134 | sys_lseek | `int(int fd, int offset, int whence)` |
| 0x154 | sys_isatty | `int(int fd)` |
| 0x158 | sys_stat | `int(const char *path, OS32_Stat *buf)` |
| 0x15C | sys_fstat | `int(int fd, OS32_Stat *buf)` |

### パス管理

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x0DC | path_get_drive | `const char *(void)` |
| 0x0E0 | path_get_cwd | `const char *(void)` |
| 0x0E4 | path_set_drive | `int(const char *d)` |
| 0x0E8 | path_set_cwd | `void(const char *p)` |
| 0x0EC | path_parse | `void(const char *input, void *result)` |
| 0x0F0 | ext2_format | `int(int drv, u32 sectors)` |

### シリアル通信

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x08C | serial_init | `void(u32 baud)` |
| 0x090 | serial_puts | `void(const char *s)` |
| 0x094 | serial_getchar | `int(void)` |
| 0x098 | serial_putchar | `void(u8 ch)` |
| 0x09C | serial_trygetchar | `int(void)` |
| 0x0A0 | serial_is_initialized | `int(void)` |

### デバイス・IDE

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x0A8 | dev_count | `int(void)` |
| 0x0AC | dev_get_info | `int(int idx, char *name, int nm, int *type, u32 *sects)` |
| 0x0CC | ide_init | `void(void)` |
| 0x0D0 | ide_drive_present | `int(int drv)` |
| 0x0D4 | ide_identify | `int(int drv, void *info)` |
| 0x0D8 | ide_get_info | `int(int drv, void *info)` |
| 0x280 | dev_blk_read | `int(const char *dev_name, u32 lba, int count, void *buf)` |
| 0x284 | dev_blk_write | `int(const char *dev_name, u32 lba, int count, const void *buf)` |

### サウンド

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x0B0 | fm_startup_sound | `void(void)` |
| 0x0B4 | fm_play_mml | `void(const char *mml)` |
| 0x0FC | buz_on | `void(void)` |
| 0x100 | buz_off | `void(void)` |
| 0x1D8 | snd_bgm_play | `void(const char *mml)` |
| 0x1DC | snd_bgm_stop | `void(void)` |
| 0x1E0 | snd_bgm_is_playing | `int(void)` |
| 0x1E4 | snd_se_play | `void(int se_id)` |
| 0x1E8 | snd_se_play_raw | `void(int note, int duration_ticks, int tone)` |
| 0x1EC | snd_set_master | `void(int enable)` |
| 0x1F0 | snd_bgm_set_persist | `void(int persist)` |
| 0x1F8 | fm_note_on | `void(int ch, int note)` |
| 0x1FC | fm_note_off | `void(int ch)` |
| 0x200 | fm_set_tone_num | `void(int ch, int tone_num)` |
| 0x204 | ssg_tone | `void(int ch, u16 period)` |
| 0x208 | ssg_volume | `void(int ch, u8 vol)` |
| 0x20C | ssg_all_off | `void(void)` |

### NP2 エミュレータ連携

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x0B8 | np2_detect | `int(void)` |
| 0x0BC | np2_get_version | `void(char *buf, int size)` |
| 0x0C0 | np2_get_cpu | `void(char *buf, int size)` |
| 0x0C4 | np2_get_clock | `void(char *buf, int size)` |
| 0x0C8 | np2_check_hostdrv | `int(char *buf, int size)` |

### KCG (漢字フォント)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x0F4 | kcg_init | `void(void)` |
| 0x0F8 | kcg_set_scale | `void(int s)` |
| 0x17C | kcg_read_ank | `void(u8 ch, u8 *buf)` |
| 0x180 | kcg_read_kanji | `void(u16 jis_code, u8 *buf)` |
| 0x258 | kcg_load_font | `int(const char *path)` |

### その他システム

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x104 | rshell_set_active | `void(int active)` |

### IME (日本語入力)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x190 | ime_getchar | `int(void)` |
| 0x194 | ime_trygetchar | `int(void)` |
| 0x198 | ime_toggle | `void(void)` |
| 0x19C | ime_is_active | `int(void)` |
| 0x1A0 | ime_set_mode | `void(int mode)` |
| 0x1A4 | ime_get_mode | `int(void)` |
| 0x1A8 | ime_getkey | `int(void)` |

### FDリダイレクト・パイプ

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x1AC | sys_redirect_fd | `int(int fd, const char *path, int mode)` |
| 0x1B0 | sys_reset_redirect | `void(int fd)` |
| 0x1B4 | sys_is_redirected | `int(int fd)` |
| 0x1B8 | sys_pipe_alloc | `int(void)` |
| 0x1BC | sys_pipe_free | `void(int id)` |
| 0x1C0 | sys_pipe_get_buf | `u8 *(int id)` |
| 0x1C4 | sys_pipe_get_len | `u32(int id)` |
| 0x1C8 | sys_pipe_clear | `void(int id)` |
| 0x1CC | sys_redirect_fd_buf | `int(int fd, u8 *buf, u32 size, u32 len)` |
| 0x1D0 | sys_redirect_get_buf_len | `u32(int fd)` |

### マウス

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x210 | mouse_poll | `void(void *info)` |
| 0x214 | mouse_available | `int(void)` |
| 0x218 | mouse_set_bounds | `void(i16 x_min, i16 y_min, i16 x_max, i16 y_max)` |
| 0x224 | mouse_cursor_set_mode | `void(int mode)` |
| 0x228 | mouse_cursor_show | `void(void)` |
| 0x22C | mouse_cursor_hide | `void(void)` |

### DB API (SQLite)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x230 | db_open | `int(const char *path)` |
| 0x234 | db_close | `int(int handle)` |
| 0x238 | db_exec | `int(int handle, const char *sql)` |
| 0x23C | db_prepare | `int(int handle, const char *sql)` |
| 0x240 | db_step | `int(int handle)` |
| 0x244 | db_column_int | `int(int handle, int col)` |
| 0x248 | db_column_text | `const char *(int handle, int col)` |
| 0x24C | db_finalize | `int(int handle)` |
| 0x250 | db_last_error | `const char *(int handle)` |
| 0x254 | db_mem_used | `u32(void)` |

### V86 サブシステム

| Offset | フィールド | プロトタイプ | 備考 |
|--------|-----------|------|------|
| 0x25C | sys_v86_boot_freedos | `int(const char *path, const char *cmdline)` | ⚠️ **廃止予定** (Phase F) |
| 0x260 | sys_v86_boot_physical | `int(int drv, const char *cmdline)` | |
| 0x264 | sys_v86_boot_physical_ex | `int(int drv, int media, const char *cmdline)` | |
| 0x268 | sys_v86_boot_native | `int(const char *path, const char *cmdline)` | |
| 0x26C | sys_v86_set_debug | `void(int enabled)` | |
| 0x288 | sys_v86_boot_image | `int(const char *path, const char *cmdline)` | v38 追加 |

### ループデバイス・ブロックI/O

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x274 | loop_attach | `int(const char *path, int slot)` |
| 0x278 | loop_detach | `void(int slot)` |
| 0x27C | loop_status | `int(int slot, u32 *total, int *bps)` |

---

## データフィールド (構造体末尾)

| Offset | フィールド | 型 | 説明 |
|--------|-----------|------|------|
| ※末尾 | sbrk_heap_limit | `u32` | newlib _sbrk用ヒープ上限アドレス (exec_runでセットされる) |

> **注**: sbrk_heap_limit のオフセットは API 関数数に依存して変動します。

---

*Last Updated: 2026-05-11*
