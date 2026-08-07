# KernelAPI v35 仕様書

外部プログラム (OS32X) がカーネル機能を利用するためのAPIテーブル仕様。

---

## §1 概要

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| KAPIテーブルアドレス | 動的算出 (KHEAP_BASE + KHEAP_SIZE) |
| KAPIマジック | 0x4B415049 ('KAPI') |
| プログラムロード先 | 0x400000 |
| 最大プログラムサイズ | 1MB |
| プログラム専用ヒープ | 動的配置 (sbrk_heap_limit, exec_heap 管理下) |
| プログラム専用スタック | 動的配置 (メモリ終端付近、下向き展開) |
| 現在のバージョン | **35** |
| 合計エントリ数 | **168** (ヘッダ2 + 関数ポインタ164 + データフィールド2) |

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

### §3-1 KernelAPI 関数の追加手順

**`tools/kapi.json` が唯一の情報源 (SSOT)。** 構造体・ラッパー・初期化コード・Rust
バインディングはすべてここから生成されるので、生成物を直接編集してはならない。

1. `tools/kapi.json` の `api` 配列の**末尾**にエントリを追加する。
   既存スロットの並べ替え・削除は**禁止** (ビルド済みバイナリの ABI が壊れる)。
   必要なヘッダは同ファイルの `includes` に、プロトタイプは `externs` に追加する。
   - `target` — 実体の関数名がエントリ名と異なる場合に指定
   - `body` — ラッパー本体をインラインで書く場合に指定
2. `tools/kapi.json` の `"version"` と `include/os32_kapi_shared.h` の
   `KAPI_VERSION` を**両方**インクリメントする (一致必須)。
3. 再生成して差分が意図した追加のみであることを確認する:
   ```bash
   python3 tools/gen_kapi.py && python3 tools/kapi_rust_gen.py
   git diff --stat
   ```
   生成対象: `include/os32_kapi_generated.h` / `kapi/kapi_generated.c` /
   `exec/exec_kapi_init.inc` / `programs/rust/os32api/src/kapi_generated.rs`
4. カーネル側に実体を実装する。
5. 本仕様書のオフセット表を更新し、新APIに依存するプログラムの
   `build/app.conf` の要求バージョンを引き上げる。

値を持つフィールド (関数ポインタでないもの) は `data_fields` に追加する。
ジェネレータは `kapi-><field> = 0;` を出力するだけなので、実際の値は
`exec/exec.c` の `exec_init()` (または `exec_run()`) で代入すること。

---

## §4 KernelAPI 構造体レイアウト

### ヘッダ

| Offset | フィールド | 説明 |
|--------|-----------|------|
| 0x00 | magic | 0x4B415049 ("KAPI") |
| 0x04 | version | APIバージョン (現在: 35) |

### API関数 (自動生成 — os32_kapi_generated.h 準拠)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x08 | gfx_init | `void(void)` |
| 0x0C | gfx_init_200 | `void(void)` |
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
| 0x11C | shell_print_utf8 | `void(const char *utf8_str, u8 color)` |
| 0x120 | console_get_cursor_x | `int(void)` |
| 0x124 | console_get_cursor_y | `int(void)` |
| 0x128 | console_set_cursor | `void(int x, int y)` |
| 0x12C | sys_open | `int(const char *path, int mode)` |
| 0x130 | sys_close | `void(int fd)` |
| 0x134 | sys_read | `int(int fd, void *buf, u32 size)` |
| 0x138 | sys_write | `int(int fd, const void *buf, u32 size)` |
| 0x13C | sys_lseek | `int(int fd, int offset, int whence)` |
| 0x140 | console_get_size | `void(int *w, int *h)` |
| 0x144 | kbd_get_modifiers | `u32(void)` |
| 0x148 | sys_get_mem_kb | `u32(void)` |
| 0x14C | sys_time | `os_time_t(void)` |
| 0x150 | gfx_hardware_scroll | `void(int lines)` |
| 0x154 | gfx_present_rect | `void(int x, int y, int w, int h)` |
| 0x158 | sys_exit | `void(int status)` |
| 0x15C | sys_isatty | `int(int fd)` |
| 0x160 | sys_stat | `int(const char *path, OS32_Stat *buf)` |
| 0x164 | sys_fstat | `int(int fd, OS32_Stat *buf)` |
| 0x168 | gfx_set_palette | `void(int idx, u8 r, u8 g, u8 b)` |
| 0x16C | gfx_get_palette | `void(int idx, u8 *r, u8 *g, u8 *b)` |
| 0x170 | gfx_get_framebuffer | `void(void *fb)` |
| 0x174 | gfx_add_dirty_rect | `void(int x, int y, int w, int h)` |
| 0x178 | gfx_present_dirty | `void(void)` |
| 0x17C | gfx_present_nosync | `void(void)` |
| 0x180 | gfx_present_raster | `void(void *table)` |
| 0x184 | kcg_read_ank | `void(u8 ch, u8 *buf)` |
| 0x188 | kcg_read_kanji | `void(u16 jis_code, u8 *buf)` |
| 0x18C | sys_shm_alloc | `void *(int blocks)` |
| 0x190 | sys_shm_lock | `int(void *ptr)` |
| 0x194 | sys_shm_free | `int(void *ptr)` |
| 0x198 | ime_getchar | `int(void)` |
| 0x19C | ime_trygetchar | `int(void)` |
| 0x1A0 | ime_toggle | `void(void)` |
| 0x1A4 | ime_is_active | `int(void)` |
| 0x1A8 | ime_set_mode | `void(int mode)` |
| 0x1AC | ime_get_mode | `int(void)` |
| 0x1B0 | ime_getkey | `int(void)` |
| 0x1B4 | sys_redirect_fd | `int(int fd, const char *path, int mode)` |
| 0x1B8 | sys_reset_redirect | `void(int fd)` |
| 0x1BC | sys_is_redirected | `int(int fd)` |
| 0x1C0 | sys_pipe_alloc | `int(void)` |
| 0x1C4 | sys_pipe_free | `void(int id)` |
| 0x1C8 | sys_pipe_get_buf | `u8 *(int id)` |
| 0x1CC | sys_pipe_get_len | `u32(int id)` |
| 0x1D0 | sys_pipe_clear | `void(int id)` |
| 0x1D4 | sys_redirect_fd_buf | `int(int fd, u8 *buf, u32 size, u32 len)` |
| 0x1D8 | sys_redirect_get_buf_len | `u32(int fd)` |
| 0x1DC | paging_is_present | `int(u32 addr)` |
| 0x1E0 | snd_bgm_play | `void(const char *mml)` |
| 0x1E4 | snd_bgm_stop | `void(void)` |
| 0x1E8 | snd_bgm_is_playing | `int(void)` |
| 0x1EC | snd_se_play | `void(int se_id)` |
| 0x1F0 | snd_se_play_raw | `void(int note, int duration_ticks, int tone)` |
| 0x1F4 | snd_set_master | `void(int enable)` |
| 0x1F8 | snd_bgm_set_persist | `void(int persist)` |
| 0x1FC | kbd_is_pressed | `int(int scancode)` |
| 0x200 | fm_note_on | `void(int ch, int note)` |
| 0x204 | fm_note_off | `void(int ch)` |
| 0x208 | fm_set_tone_num | `void(int ch, int tone_num)` |
| 0x20C | ssg_tone | `void(int ch, u16 period)` |
| 0x210 | ssg_volume | `void(int ch, u8 vol)` |
| 0x214 | ssg_all_off | `void(void)` |
| 0x218 | mouse_poll | `void(void *info)` |
| 0x21C | mouse_available | `int(void)` |
| 0x220 | mouse_set_bounds | `void(i16 x_min, i16 y_min, i16 x_max, i16 y_max)` |
| 0x224 | tvram_readchar_at | `void(int x, int y, u16 *code, u8 *attr)` |
| 0x228 | tvram_reverse_cell | `int(int x, int y)` |
| 0x22C | mouse_cursor_set_mode | `void(int mode)` |
| 0x230 | mouse_cursor_show | `void(void)` |
| 0x234 | mouse_cursor_hide | `void(void)` |

### DB API (v29-v31)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x238 | db_open | `int(const char *path)` |
| 0x23C | db_close | `int(int handle)` |
| 0x240 | db_exec | `int(int handle, const char *sql)` |
| 0x244 | db_prepare | `int(int handle, const char *sql)` |
| 0x248 | db_step | `int(int handle)` |
| 0x24C | db_column_int | `int(int handle, int col)` |
| 0x250 | db_column_text | `const char *(int handle, int col)` |
| 0x254 | db_finalize | `int(int handle)` |
| 0x258 | db_last_error | `const char *(int handle)` |
| 0x25C | db_mem_used | `u32(void)` |

### フォント (v32)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x260 | kcg_load_font | `int(const char *path)` |

### デバイス / ループバック / システム情報 (v33)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x264 | ide_get_info | `int(int drv, void *info)` |
| 0x268 | sys_get_build_info | `void(char *buf, int size)` |
| 0x26C | loop_attach | `int(const char *path, int slot)` |
| 0x270 | loop_detach | `void(int slot)` |
| 0x274 | loop_status | `int(int slot, u32 *total, int *bps)` |
| 0x278 | dev_blk_read | `int(const char *dev_name, u32 lba, int count, void *buf)` |
| 0x27C | dev_blk_write | `int(const char *dev_name, u32 lba, int count, const void *buf)` |

### FEP 辞書管理 / ノンブロッキング入力 (v35)

| Offset | フィールド | プロトタイプ |
|--------|-----------|------|
| 0x280 | ime_switch_dict | `int(int variant)` |
| 0x284 | ime_user_list | `int(const char *yomi_prefix, void *out, int max)` |
| 0x288 | ime_user_delete | `int(const char *yomi, const char *kanji)` |
| 0x28C | ime_user_export | `int(const char *path)` |
| 0x290 | ime_user_clear | `int(void)` |
| 0x294 | ime_trygetkey | `int(void)` |

`ime_user_list` の `out` は `IME_UserEntry`(`include/os32_kapi_shared.h`) の配列。
`ime_trygetkey` は FEP を通したノンブロッキングのキー取得で、`kbd_trygetkey` の
FEP 対応版にあたる (エディタ等のメインループから使う)。

### データフィールド (構造体末尾)

関数ポインタではなく値を持つフィールド。ジェネレータは `kapi-><field> = 0;` を
出力するだけなので、**実際の値は `exec_init()` / `exec_run()` で代入する**。

| Offset | フィールド | 型 | 説明 |
|--------|-----------|------|------|
| 0x298 | sbrk_heap_limit | `u32` | newlib _sbrk用ヒープ上限アドレス (exec_runでセットされる) |
| 0x29C | shm_base | `u32` | 共有メモリ (MEM_SHM_BASE) の先頭アドレス。DB結果受け渡しに使用 (exec_initでセット)。`MEM_SHM_BASE` は `__bss_end` 由来で可変なため、ユーザ空間はアドレスをハードコードしてはならない |

### §4-1 グラフィックスAPI に関する補足

v22以降、基本的な描画プリミティブ (`gfx_clear`, `gfx_pixel`, `gfx_hline`, `gfx_vline`, `gfx_line`, `gfx_rect`, `gfx_fill_rect`) は KernelAPI から**廃止**されました。

外部プログラムでグラフィックス描画を行う場合は、以下の２つの方式から選択します:

1. **libos32gfx ライブラリ** (推奨): `programs/libos32gfx/` で提供されるスタティックリンクライブラリ。サーフェス、スプライト、描画プリミティブ、ダーティ矩形管理、フォントレンダリングなど高レベルな描画機能を提供します。
2. **フレームバッファ直接操作**: `gfx_get_framebuffer()` で取得した `GFX_Framebuffer` 構造体を介して、4プレーンのバックバッファに直接書き込み、`gfx_add_dirty_rect()` + `gfx_present_dirty()` でVRAMに転送します。

**描画モード**:

| モード | 解像度 | 初期化 | ページフリップ |
|--------|--------|--------|---------------|
| 400ラインモード | 640×400 | `gfx_init()` | 自動有効 |
| 200ラインモード | 640×200 | `gfx_init_200()` | 自動有効 |

**ページフリッピング**:
`gfx_init()` / `gfx_init_200()` いずれでもページフリッピングが自動的に有効になります。
`gfx_present_dirty()` / `gfx_present_nosync()` は非表示ページにVRAM転送後、ポートA4H/A6Hでページを切り替えます。VSYNC待ちは不要となり、ティアリングが発生しません。外部プログラム側のコード変更は不要です。

### §4-2 ラスタパレット (gfx_present_raster)

v24で追加。VSYNC後のアクティブ表示期間中に、走査線ごとにパレットレジスタを書き換えることで、16色パレットの制約を超えた擬似多色表示を実現します。

- **引数**: `GFX_RasterPalTable *table` — ラスタパレットテーブルへのポインタ
- **構造体**: `GFX_RasterPalEntry` (line, pal_idx, r, g, b) × 最大200エントリ
- **動作**: dirty rectがあればVRAM転送も行い、なければパレット書き換えのみ
- **ページフリップとの併用**: フリップモードではVRAM転送をフリップ経由で行い、
  VSYNC同期のパレット書き換えのみ実行します。両モードで動作します。
- **libos32gfx ラッパー**: `gfx_raster_clear()`, `gfx_raster_add()`, `gfx_present_raster_only()`, `gfx_present_with_raster()`

### §4-3 FDリダイレクト・パイプAPI

v25で追加。外部プログラム（シェル）がFD単位の入出力リダイレクトとパイプラインを構築するためのAPI群。

**FDリダイレクト**:
- `sys_redirect_fd(fd, path, mode)` — 指定FDの出力先をファイルにリダイレクト
- `sys_reset_redirect(fd)` — リダイレクトを解除しコンソールに復帰
- `sys_is_redirected(fd)` — FDがリダイレクト中か判定
- `sys_redirect_fd_buf(fd, buf, size, len)` — FDの出力先をメモリバッファにリダイレクト
- `sys_redirect_get_buf_len(fd)` — バッファリダイレクト時の書き込み済みバイト数取得

**パイプバッファ**:
- `sys_pipe_alloc()` — パイプバッファを1個確保 (IDを返す)
- `sys_pipe_free(id)` — パイプバッファを解放
- `sys_pipe_get_buf(id)` — パイプバッファのデータポインタ取得
- `sys_pipe_get_len(id)` — パイプバッファの書き込み済みバイト数取得
- `sys_pipe_clear(id)` — パイプバッファをクリア

**典型的なパイプ実行フロー** (`cmd1 | cmd2`):
1. `sys_pipe_alloc()` でパイプ確保
2. `sys_redirect_fd_buf(1, pipe_buf, size, 0)` でcmd1のstdoutをパイプに接続
3. cmd1を実行
4. `sys_reset_redirect(1)` でstdout復帰
5. `sys_redirect_fd_buf(0, pipe_buf, len, len)` でcmd2のstdinをパイプに接続
6. cmd2を実行
7. `sys_reset_redirect(0)` → `sys_pipe_free(id)` でクリーンアップ

### §4-4 ページング問い合わせAPI

v26で追加。指定アドレスのページテーブルエントリが存在するか (Present ビット) を確認する。

- `paging_is_present(addr)` — 指定アドレスが有効にマッピングされているか判定 (1=有効, 0=Not-Present)
- **用途**: メモリダンプツール等がガードページや未マッピング領域への不正アクセスを事前に回避するために使用

### §4-5 キー押下状態ポーリングAPI

v27で追加。指定スキャンコードのキーが現在押下中かをリアルタイムに問い合わせる。
ゲームエンジン (libpyxel) のフレーム単位入力に使用。

- `kbd_is_pressed(scancode)` — 指定スキャンコードのキーが押されていれば1、離されていれば0
- IRQハンドラで128キー分のビットマップを常時更新しているため、イベントキューを消費しない

### §4-6 FM/SSG個別チャンネル制御API

v27で追加。FM音源(YM2203)の3チャンネルおよびSSG(PSG)の3チャンネルを個別に制御する低レベルAPI。
ゲームエンジンのサウンドシーケンサ実装に使用。

- `fm_note_on(ch, note)` — FMチャンネル(0-2)でノート発音
- `fm_note_off(ch)` — FMチャンネル消音
- `fm_set_tone_num(ch, tone_num)` — FMチャンネルのプリセット音色設定
- `ssg_tone(ch, period)` — SSGチャンネル(0-2)のトーン周期設定
- `ssg_volume(ch, vol)` — SSGチャンネルの音量設定(0-15)
- `ssg_all_off()` — SSG全チャンネル消音

### §4-7 マウスAPI

v28で追加。PC-98バスマウスおよびNP21/Wシームレスマウスに対応するポーリングベースのマウスAPI。

- `mouse_poll(info)` — `MouseInfo` 構造体に現在の座標・差分・ボタン状態を取得
- `mouse_available()` — マウスが使用可能か判定 (1=バスマウス, 2=シームレス, 0=なし)
- `mouse_set_bounds(x_min, y_min, x_max, y_max)` — マウス座標のクランプ範囲を設定

**MouseInfo 構造体**:
- `x`, `y` — 現在の画面座標
- `dx`, `dy` — 前回poll以降の差分
- `buttons` — ボタンビットマスク (`MOUSE_BTN_LEFT`=0x01, `MOUSE_BTN_RIGHT`=0x02, `MOUSE_BTN_MIDDLE`=0x04)
- `mode` — 動作モード (0=なし, 1=バス, 2=シームレス)

### §4-8 TVRAM読取・反転API

v28で追加。テキストVRAMの読み取りと属性操作を行う。マウスカーソル (テキストモード) の実装に使用。

- `tvram_readchar_at(x, y, *code, *attr)` — TVRAM 1セルの文字コード＋属性を読み取る
- `tvram_reverse_cell(x, y)` — 属性反転トグル (PC-98属性ビット2 (0x04) のXOR)。漢字2セル自動対応。戻り値=セル幅 (ANK=1, 漢字=2)

### §4-9 マウスカーソル制御API

v28で追加。カーネル管理のマウスカーソル表示を制御する。アプリケーションはカーソル描画を自前で行う必要がなくなる。

- `mouse_cursor_set_mode(mode)` — カーソルモード設定
  - `MOUSE_CURSOR_NONE` (0): カーソル非表示 (生ポーリング専用)
  - `MOUSE_CURSOR_TEXT` (1): TVRAM属性反転カーソル
  - `MOUSE_CURSOR_GFX` (2): GFXスプライトカーソル (将来用)
- `mouse_cursor_show()` — カーソル表示
- `mouse_cursor_hide()` — カーソル非表示 (画面更新前にhide→更新→showのパターンで使用)

---

*Last Updated: 2026-04-29*
