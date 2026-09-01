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

### §5-5 グラフィック (gfx.c / gfx.h / libos32gfx)

CPU直接描画＋バックバッファ方式。カーネル層 (`gfx/`) はバックバッファ管理とVRAM転送を担当し、高レベルな描画機能は外部プログラム用ライブラリ `libos32gfx` (`userland/lib/gfx/`) に分離されている。

| 項目 | 仕様 |
|------|------|
| 解像度 | 640×400 |
| 色数 | 16色 (4プレーン) |
| 描画方式 | システムRAMバックバッファ → VRAM一括転送 |
| プレーンサイズ | 32,000バイト (80×400) |
| パレットI/O | 0xA8 (idx), 0xAA (G), 0xAC (R), 0xAE (B) |

**カーネル層 (gfx/) 主要API**:

| 関数 | 説明 |
|------|------|
| `gfx_init()` | 640x400x16初期化 + バックバッファ確保 |
| `gfx_shutdown()` | テキストモード復帰 |
| `gfx_present()` | バックバッファ全面→VRAM転送 |
| `gfx_present_rect(x,y,w,h)` | 矩形領域のみVRAM転送 |
| `gfx_present_dirty()` | ダーティ矩形のみVRAM転送 (KernelAPI経由) |
| `gfx_add_dirty_rect(x,y,w,h)` | ダーティ矩形の登録 (KernelAPI経由) |
| `gfx_get_framebuffer(fb)` | バックバッファ情報取得 (KernelAPI経由) |
| `gfx_hardware_scroll(lines)` | GDCハードウェアスクロール |
| `gfx_clear(color)` | 画面クリア (カーネル内部用) |
| `gfx_fill_rect(x,y,w,h,c)` | 矩形塗りつぶし (カーネル内部用) |
| `gfx_draw_sprite(x,y,spr)` | スプライト描画 (カーネル内部用) |

**libos32gfx (外部プログラム用ライブラリ) 主要機能**:

| モジュール | 説明 |
|------------|------|
| `gfx_draw.c` | 描画プリミティブ (pixel, hline, vline, line, rect, fill_rect) |
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
| シームレスマウス | NP21/W拡張 (NP2SysP経由) |
| 座標系 | 画面座標 (0,0)-(639,399) |
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
