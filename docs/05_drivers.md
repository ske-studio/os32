## 第5部 デバイスドライバ

### §5-1 キーボード (kbd.c / kbd.h)

PC-98キーボードコントローラ制御。IRQ1割り込みハンドラで入力を取得。

| 項目 | 仕様 |
|------|------|
| I/Oポート | 0x41 (データ), 0x43 (コマンド) |
| 割り込み | IRQ1 (INT 0x21) |
| バッファ | u16リングバッファ (256エントリ) |
| バッファ形式 | 上位8bit=スキャンコード, 下位8bit=ASCII |

**API**:

| 関数 | 説明 |
|------|------|
| `kbd_init()` | キーボード初期化、IRQ1有効化 |
| `kbd_getchar()` | ASCII文字を1文字返す (ブロッキング) |
| `kbd_getkey()` | スキャンコード+ASCII (u16) を返す |
| `kbd_has_key()` | バッファにデータがあるか |

**特殊キー (kbd_getkey()のスキャンコード)**:

| キー | scan | ASCII |
|------|------|-------|
| ↑ | 0x3A | 0x00 |
| ↓ | 0x3D | 0x00 |
| ← | 0x3B | 0x00 |
| → | 0x3C | 0x00 |
| Home | 0x3E | 0x00 |
| Del | 0x39 | 0x00 |
| Tab | - | 0x09 |
| ESC | - | 0x1B |
| BS | - | 0x08 |
| Enter | - | 0x0D |

### §5-2 フロッピーディスク (fdc.c / fdc.h)

BIOS (INT 1Bh) を使用せず、I/Oポート直接制御および DMA (μPD8237A ch2) 転送による完全なハードウェア制御を実装している。

| 項目 | 仕様 |
|------|------|
| I/Oポート | 0x90 (メインステータス `FDC_MSR`), 0x92 (データ `FDC_FIFO`), 0x94 (コントロール `FDC_CTRL`) |
| 転送方式 | DMA (μPD8237A ch2)。アドレス 0x09 / カウント 0x0B / バンク 0x23 / マスク 0x15 / モード 0x17 / F-F クリア 0x19 (`drivers/fdc.h`) |
| 割り込み | (ポーリングによるビジーウェイト制御) |
| DMAバッファ | 64KB境界をまたがない静的確保バッファ |

**API**:

| 関数 | 説明 |
|------|------|
| `fdc_init()` | FDCコントローラの初期化 |
| `fdc_read_sector(drv, cyl, head, sect, buf)` | CHS指定で1セクタ読込 |
| `fdc_write_sector(drv, cyl, head, sect, buf)` | CHS指定で1セクタ書込 |
| `disk_read_lba(lba,count,buf)` | LBA指定でセクタ読込 (ユーティリティ) |
| `disk_write_lba(lba,count,buf)` | LBA指定でセクタ書込 (ユーティリティ) |

### §5-3 FM音源 (fm.c / fm.h)

YM2203 (OPN) FM音源チップ制御。FM 3ch + SSG 3ch。

| 項目 | 仕様 |
|------|------|
| アドレスポート | 0x0188 |
| データポート | 0x018A |
| FM チャンネル | 3ch (CH1-3) |
| SSG チャンネル | 3ch (A-C) |
| SSG基準クロック | 3,993,600 Hz |

**API**:

| 関数 | 説明 |
|------|------|
| `opn_init()` | OPN初期化 |
| `fm_note_on(ch, note)` | FM発音 |
| `fm_note_off(ch)` | FM消音 |
| `fm_startup_sound()` | 起動ジングル再生 |
| `fm_play_mml(mml)` | 簡易MML文字列再生 |
| `ssg_beep()` | BEEP音 |
| `ssg_tone(ch, period)` | SSGトーン設定 |

### §5-4 RS-232C (serial.c / serial.h)

μPD8251A (USART) 内蔵シリアルポート制御。

| 項目 | 仕様 |
|------|------|
| データポート | 0x30 |
| コマンドポート | 0x32 |
| 信号ポート | 0x33 |
| 割り込みマスク | 0x35 |
| ボーレートタイマ | PIT カウンタ#2 (0x75) |
| 受信バッファ | 256バイト リングバッファ |
| IRQ | IRQ4 |

**API**:

| 関数 | 説明 |
|------|------|
| `serial_init(baud)` | 初期化 (38400bps等) |
| `serial_putchar(c)` | 1文字送信 (TxRDY待ち+hltスリープ) |
| `serial_puts(str)` | 文字列送信 (16Bごとにフロー制御) |
| `serial_getchar()` | 1文字受信 (ブロッキング) |
| `serial_trygetchar()` | 1文字受信 (ノンブロッキング) |

**フロー制御**: `serial_puts`は16バイトごとに`io_wait`を挿入し、NP21/Wのパイプバッファ溢れを防止する。

### §5-5 グラフィック (gfx/ HAL / libos32gfx)

CPU直接描画＋バックバッファ方式。カーネル層 (`gfx/`) はバックバッファ管理とVRAM転送を担当し、高レベルな描画機能は外部プログラム用ライブラリ `libos32gfx` (`userland/lib/gfx/`) に分離されている。

**HAL バックエンド表 (GUI v1.1、2026-09-06 現行)**: `include/gfx_hal.h` の `GfxBackend`
(probe / init / query / present_rect / set_palette / fill_rect / blit / enter / leave / shutdown、
`bb_base` / `bb_size` / `bb_pitch` / `bb_format`) を 3 枚持ち、`gfx_core.c` が **probe 順
Cirrus → PEGC → 9801** で最初に通ったものを使う。`/etc/system.cfg` の `GFX=pc98|pegc|cirrus|auto`
(`gfxmode` コマンドが書く) で強制できる。能力は `gfx_screen_info()` (契約 G5) で問い合わせ、
GUI もアプリも 400 ライン / 16 色 / プレーンを決め打ちしない。

| バックエンド | 画面 | バックバッファ (CPU が描く面) | 表示面へ | 能力ビット |
|---|---|---|---|---|
| `backend_pc98.c` (9801) | 640×400×16 (4 プレーン) | 主記憶 0x6A000 (128KB、`MEM_GFX_BB_BASE`) | CPU 転送 + ページフリップ | TEXT_OVERLAY, PAGE_FLIP |
| `backend_pegc.c` (9821 PEGC) | 640×480×256 (PACKED8) | 主記憶末尾から 300KB (`sys_reserve_top`) | CPU 転送 (F00000h リニア窓、GDC SYNC は BIOS 表から 480 ライン) | TEXT_OVERLAY (合成ありと実測) |
| `backend_cirrus.c` (CL-GD5430、Xe10 内蔵) | 640×480×256 | カード VRAM のクライアント面 (リニア窓 01000000h + 04B000h、300KB) | エンジン BLT (`present_rect` = 非表示面 → 表示面)。塗り / 転送も HW、256 画素以下は CPU 直書き | HW_FILL, HW_BLT (映像はリレーで切替、TEXT_OVERLAY 無し) |

デバイス窓は master PD に **supervisor + PCD** で張り、CPL=3 に見せるのはクライアント面だけ
(表示面へ直接描かせない = 契約 G4。写像の規則は [02 §2-1](02_memory.md))。counters
(`gfx_stats`: present_bytes / hw_ops / io_accesses / commits) で「CPU が運んだ量」と
「エンジンに任せた回数」を測る (NP21/W では時間が測れないため)。設計と経緯は
[tasks/gui/DESIGN.md §5〜§8](tasks/gui/DESIGN.md)、Cirrus の資料は `include/wab_xe10.h` と
`drivers/wab_*`。NP21/W では Cirrus は ini の `USEGD5430=true` / `GD5430TYPE=91` (Xe10) を要する ([D2])。

| 項目 | 仕様 (9801 バックエンド) |
|------|------|
| 解像度 | 640×400 (`gfx_init`) / 640×200 (`gfx_init_200`、縦は HW が 2 倍表示) |
| 色数 | 16色 (4プレーン) |
| ページフリップ | 両モードで自動有効 (ポート A4h = 表示ページ / A6h = アクセスページ) |
| 描画方式 | システムRAMバックバッファ → VRAM一括転送 |
| プレーンサイズ | 32,000バイト (80×400) |
| パレットI/O | 0xA8 (idx), 0xAA (G), 0xAC (R), 0xAE (B)。PEGC / Cirrus はバックエンドの `set_palette` (256 色、下位 16 はシステム色) |

**ページフリッピング** (DEVELOPMENT.md から 2026-09-05 に移動):
PC-9801 の VRAM は各プレーンに物理 2 バンク (64KB) あり、A4h (表示) / A6h (アクセス) で
切り替えられる。非表示ページへ BB から転送してから切り替える (約 3µs) ので、ティアリングが
無く、表示中ページと CPU の書き込み先が異なるためバス競合も起きない。VSYNC 待ちループ
(最大 17ms) は不要。前フレームのダーティ矩形を次フレームにマージする 2 フレーム追跡で
ステイルページを防ぐ。`gfx_present_raster()` はフリップ転送後に VSYNC 同期でパレット
だけ書き換えるのでフリップと併用できる。外部プログラムへの影響は無い (BB ポインタ不変)。
ハードウェア制約 ([HW1] EGC/GRCG/GDC 描画禁止) の理由は [POLICY_DEV.md §3](POLICY_DEV.md)。

**カーネル層 (gfx/) 主要API**:

| 関数 | 説明 |
|------|------|
| `gfx_init()` | 640x400x16初期化 + バックバッファ確保 |
| `gfx_shutdown()` | テキストモード復帰 |
| `gfx_present()` | バックバッファ全面→VRAM転送 |
| `gfx_present_rect(x,y,w,h)` | 矩形領域のみVRAM転送 |
| `gfx_present_dirty()` | ダーティ矩形のみVRAM転送 (KernelAPI経由) |
| `gfx_add_dirty_rect(x,y,w,h)` | ダーティ矩形の登録 (KernelAPI経由) |
| `gfx_get_framebuffer(fb)` | バックバッファ情報取得 (KernelAPI経由)。パックド系は `planes[0]` だけ、`planes[1..3]=NULL` |
| `gfx_screen_info(si)` / `gfx_hw_fill_rect` / `gfx_hw_blit` / `gfx_stats` / `gfx_lease_palette` | HAL の問い合わせ / HW 塗り・転送 (無いバックエンドは `OS32_ERR_NOSYS`、`gfx_init` 前も NOSYS) / カウンタ / パレットリース (KernelAPI v40〜v42) |
| `gfx_hardware_scroll(lines)` | GDCハードウェアスクロール |
| `gfx_clear(color)` | 画面クリア (カーネル内部用) |
| `gfx_fill_rect(x,y,w,h,c)` | 矩形塗りつぶし (カーネル内部用) |
| `gfx_draw_sprite(x,y,spr)` | スプライト描画 (カーネル内部用) |

**libos32gfx (外部プログラム用ライブラリ) 主要機能**:

| モジュール | 説明 |
|------------|------|
| `libos32gfx_core.c` | `libos32gfx_init` (gfx_init + attach) / `libos32gfx_attach` (gshell 配下のアプリと shlib: framebuffer 取り直し + **画素形式 `gfx_packed` の判定** + プール初期化。判定をここに集約しないと PACKED8 で漢字が描けない、2026-09-06) |
| `gfx_draw.c` | 描画プリミティブ (pixel, hline, vline, line, rect, fill_rect)。4 プレーンと PACKED8 の両経路 (`gfx_packed`) |
| `gfx_surface.c` | サーフェス管理 (create, free, clear, pixel, fill_rect) |
| `gfx_sprite.c` | スプライト管理 (create, free, draw, 背景退避/復帰) |
| `gfx_blt.c` | ブリット (矩形の退避/復帰) |
| `gfx_font.c` | KCGフォントレンダリング (ANK/漢字/UTF-8文字列) |
| `gfx_kcg.c` | KCGフォント読み出し (ANK/漢字) |
| `gfx_circle.c` | 円/楕円/円弧/太線円の描画 |
| `gfx_bezier.c` | ベジェ曲線 (2次/3次/太線、de Casteljau整数演算) |
| `gfx_math.c` | 整数sin/cos 互換ラッパー (実体は libos32math/trig.c) |
| `gfx_raster.c` | ラスタパレット管理 (clear, add, present) |
| `gfx_dump.c` | VRAM＋パレットのBMPダンプ出力 |
| `gfx_util.asm` | NASM高速ユーティリティ (memcpy/memset最適化等) |
| `lconsole.c` | グラフィックモード用論理コンソール |

### §5-6 ATAPI CD-ROM (atapi.c / atapi.h)

IDEセカンダリバンクに接続されたATAPI CD-ROMデバイスをPIOモードで制御する。SCSIコマンド (CDB) をPACKETコマンド経由で発行し、セクタ読み出しを行う。

| 項目 | 仕様 |
|------|------|
| I/Oポート | タスクファイル 0x640-0x64E、代替ステータス/デバイス制御 0x74C。バンク切替は 0x430/0x432 (`IDE_BANK0`/`IDE_BANK1`) で、ここはポートではなくプライマリ/セカンダリの選択に使う (`drivers/ide.h`) |
| コマンドプロトコル | ATA PACKET (0xA0) + SCSI CDB |
| セクタサイズ | 2048バイト |
| アドレッシング | LBA (READ(10) CDB) |
| 対応CDB | TEST UNIT READY, READ CAPACITY, READ(10) |

**API**:

| 関数 | 説明 |
|------|------|
| `atapi_init()` | CD-ROM検出 (セカンダリバンクのATAPIシグネチャ確認) |
| `atapi_present()` | CD-ROMドライブ存在チェック |
| `atapi_test_unit_ready()` | メディア挿入確認 |
| `atapi_read_capacity(cap)` | メディア容量取得 (AtapiCapacity構造体) |
| `atapi_read_sectors(lba, count, buf)` | セクタ読み出し (2048B/セクタ, LBA指定) |

### §5-7 マウス (mouse.c / mouse.h / mouse_bus.c / mouse_seamless.c)

PC-98バスマウスおよびNP21/Wシームレスマウスに対応するポーリングベースのマウスドライバ。
カーネル初期化時に自動検出し、利用可能なモードを選択する。

| 項目 | 仕様 |
|------|------|
| バスマウスI/O | 0x7FD9 (データ `MOUSE_DATA`), 0x7FDD (制御 `MOUSE_CTRL`), 0x7FDF (8255 モード `MOUSE_MODE`)。IRQ13 (スレーブ PIC IR5, INT 0x2D) |
| シームレスマウス | NP21/W拡張 (NP2SysP `getmpos`)。ホストカーソルの絶対座標 (0..65535) を `mouse_poll` ごとに読み、**現在の移動範囲 (`mouse_set_bounds`) へ比例配分**する (2026-09-06 まで 639/399 決め打ちで 480 ラインの下端に届かなかった)。ボタンは両モードとも 8255 ポート A から読む |
| 座標系 | 画面座標 (0,0)-(639,399)。gshell は PEGC / Cirrus で bounds を (639,479) にする |
| 検証 (NP21/W) | ai-debug の `POST /api/mouse`: シームレスには `ax`/`ay` (0..65535 の絶対座標の上書き、`abs=off` で解除)、バスマウスには `dx`/`dy`、ボタンは `btn`/`hold` (両モード共通) |
| ボタン | 左/右/中 (3ボタン) |
| ポーリング方式 | `mouse_poll()` でフレーム単位取得 |

**API**:

| 関数 | 説明 |
|------|------|
| `mouse_init()` | マウス検出・初期化 (シームレス優先、フォールバックでバスマウス) |
| `mouse_poll(info)` | 現在の座標・差分・ボタン状態を `MouseInfo` に取得 |
| `mouse_available()` | マウスの利用可能状態 (0=なし, 1=バス, 2=シームレス) |
| `mouse_set_bounds(x_min, y_min, x_max, y_max)` | 座標クランプ範囲設定 |

**マウスカーソル (カーネル管理)**:

| 関数 | 説明 |
|------|------|
| `mouse_cursor_set_mode(mode)` | カーソルモード設定 (NONE/TEXT/GFX) |
| `mouse_cursor_show()` | カーソル表示 |
| `mouse_cursor_hide()` | カーソル非表示 (画面更新時のhide/showパターン) |

TEXTモードではTVRAM属性反転 (ビット2 XOR) によるカーソル表示を行う。漢字2セル境界を自動検出し、左半分から反転する。

---

### §5-8 RTC (rtc.c / rtc.h)

µPD4990A カレンダ時計をI/Oポート直接制御で操作する (PC9800Bible §2-4, §4-3)。

| 項目 | 仕様 |
|------|------|
| I/Oポート | 書込み 0x20 (`RTC_SET` — DI/CLK/STB/C2-C0 ビット制御)、読出し 0x33 (`RTC_READ` — bit0 = CDAT = DATA OUT) |
| 読み出し | レジスタシフトモードでシリアル読み出し |
| API | `rtc_read(rtc_time)` — 年月日・時分秒を取得 (KernelAPI 経由でも公開) |

### §5-9 漢字キャラクタジェネレータ (kcg.c / kcg.h)

PC-98内蔵フォントROMから文字パターンを読み出す。起動時にコンベンショナルメモリ
(0x01000〜) へフォントキャッシュを構築する。

| 項目 | 仕様 |
|------|------|
| I/Oポート | 0xA1 (JIS下位), 0xA3 (JIS上位-0x20), 0xA5 (ライン+L/R), 0xA9 (パターン) |
| 対応文字 | ANK (8×16) / JIS第1・第2水準漢字 (16×16) |
| API | `kcg_init()`, `kcg_read_ank(ch, buf)`, `kcg_read_kanji(jis, buf)`, `kcg_set_scale(s)` |

### §5-10 NP21/W 通信 (np2sysp.c / np2sysp.h)

NP21/W エミュレータの np2sysp 拡張ポートと通信し、エミュレータ検出・バージョン取得・
HostDrv 状態確認を行う。

| 項目 | 仕様 |
|------|------|
| I/Oポート | 0x7EF (文字列コマンド/レスポンス), 0x7ED (32bit値シフトレジスタ) |
| API | `np2_detect()`, `np2_get_version()`, `np2_get_cpu()`, `np2_get_clock()`, `np2_check_hostdrv()` |

### §5-11 デバイス抽象化層 (dev.c / disk.c)

- `dev.c` — ブロック/キャラクタデバイスの登録・列挙 (`dev_count`, `dev_get_info`)。初期登録: `fdd0` (2HD), `con` (コンソール)。IDE 検出時に `hd0`〜 が追加される
- `disk.c` — FDD セクタI/Oユーティリティ (`disk_read_lba` / `disk_write_lba`)。FDC ドライバ経由で BIOS 不使用 (PM PIO)

### §5-12 整数数学ライブラリ (libos32math)

FPU非依存の整数数学ライブラリ。KernelAPIへの依存なし。純粋C89整数演算のみで構成され、外部プログラムライブラリ群の最も基底に位置する。

| モジュール | 説明 |
|------------|------|
| `fix16.c` | Q16.16固定小数点四則演算 (64bit中間値でオーバーフロー防止) |
| `trig.c` | sin/cos LUT (512エントリ, 15bit精度, 値域 -32767～+32767) |
| `sqrt.c` | 整数平方根 (ニュートン法) + 高速距離近似 (α-max-plus-β-min) |
| `atan2.c` | CORDIC方式整数atan2 (8回反復, シフト+加算のみ) |
| `recip.c` | 逆数LUT 257エントリ (b=1～256の高速除算) |
| `random.c` | xorshift32擬似乱数 (周期 2^32-1) |
| `vec2.c` | 2Dベクトル演算 (Q16.16ベース, 12関数) |
| `lerp.c` | 線形補間 + 7種イージング関数 |

**依存関係**:

```
libos32math  (依存なし — 最も基底のライブラリ)
     ↑
     ├── libos32gfx   (math + KAPI)
     ├── libos32snd   (math + KAPI)
     ├── libos32tilemap (math + gfx)
     └── ゲーム本体    (math + 任意のlib)
```

**リソース使用量**: 合計約4KB (コード~1.6KB + LUTデータ~2.3KB)。

詳細は [LIBMATH_DESIGN.md](tasks/libmath/LIBMATH_DESIGN.md) を参照。

---
