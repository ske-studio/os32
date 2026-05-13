# 8. セッション管理・ブートシーケンス

ソース: [`v86_session.c`](../../../kernel/v86_session.c)

## 8.1 セッション構造体

```c
typedef struct {
    enum v86_exit_reason exit_reason;  /* 終了理由 */
    int fd;                            /* イメージファイルディスクリプタ */
    u32 img_data_size;                 /* イメージデータサイズ */
    const char *auto_cmd;              /* Auto-Typerコマンド文字列 */
    int auto_cmd_idx;                  /* 次に注入する文字位置 */
    int auto_done;                     /* 注入完了フラグ */
    int auto_delay_remaining;          /* 初期ディレイ残りtick */
} V86Session;
```

## 8.2 ブートパス一覧

### v86_boot_image_kapi(path, cmdline)

ディスクイメージファイルからブート。KAPIからの呼び出しエントリポイント。

```
入力: path = イメージファイルパス ("/host/Ys.D88" 等)
      cmdline = Auto-Typerコマンド (NULL可)
```

### v86_boot_physical_fdd(drv, cmdline)

NP21/Wにマウント中の実FDDからブート。デフォルト2HD。

### v86_boot_physical_fdd_ex(drv, media, cmdline)

実FDDからブート。メディア種別指定:
- media=0: 2HD (1.2MB)
- media=1: 2DD (640KB)
- media=2: 2DD (720KB)

## 8.3 ブートシーケンス詳細

```
v86_boot_image()
  │
  ├─ (1) 再入禁止ガード: v86_active == 1 なら return -2
  │
  ├─ (2) セッション初期化: V86Session をゼロクリア
  │       auto_cmd, auto_delay_remaining を設定
  │
  ├─ (3) ネイティブモード設定
  │       v86_timeout_ticks = 0  (タイムアウト無効)
  │       v86_native_mode = 1    (VSYNC仮想化有効)
  │
  ├─ (4) イメージオープン + loop_devアタッチ
  │       v86_open_image_to_loop()
  │       ├── vfs_open(path)
  │       ├── 拡張子からフォーマット判定 (.d88/.fdi/.hdi/.raw)
  │       ├── loop_dev_attach_fd(fd, slot, fmt)
  │       └── v86_disk_attach_loop(slot) → ジオメトリ取得
  │
  ├─ (5) V86メモリ空間構築
  │       v86_mem_setup()
  │       ├── バッキングRAM確保 (640KB)
  │       ├── ページテーブルリマップ
  │       ├── IVT/BDA/メモリスイッチ初期化
  │       ├── IOビットマップ設定
  │       └── 画面初期化 (GVRAM/TVRAMクリア, パレット)
  │
  ├─ (6) デバイス仮想化初期化
  │       v86_pic_init()       → 仮想PICレジスタリセット
  │       v86_pit_init()       → 仮想PITレジスタリセット (100Hz)
  │       v86_fdc_virt_init()  → 仮想FDCステートマシンリセット
  │       v86_dma_init()       → 仮想DMAレジスタリセット
  │       v86_vsync_init()     → VSYNC armed/カウンタリセット
  │
  ├─ (7) IPLロード
  │       loop_dev_read_chs(slot, 0, 0, 1, ipl_buf)
  │       → 0x1FC0:0000 にコピー
  │
  ├─ (8) 画面強制有効化
  │       OUT 0x68, 0x0F  (ディスプレイコントロール有効)
  │       OUT 0xA2, 0x0D  (グラフィック表示制御)
  │       OUT 0x6A, 0x01  (モードフリップフロップ)
  │       OUT 0x68, 0x08  (カラーモード)
  │
  ├─ (9) v86_session_run_core()  ★ V86実行コア ★
  │       ├── TVRAM退避
  │       ├── TSS ESP0切替 (v86_kstack に変更)
  │       ├── FM音源退避+リセット (SSG/FM全Key-OFF)
  │       ├── メモリスイッチSW4設定 (サウンドボード有)
  │       ├── サウンドBIOS ROM確認ログ
  │       ├── PIC IMR確認 (IRQ0アンマスク保証)
  │       ├── exec_setjmp() → 復帰点設定
  │       ├── v86_enter(&ctx) → IRETD → V86モード ════╗
  │       │                                            ║
  │       │   [V86実行中: #GP / HW IRQ のループ]       ║
  │       │                                            ║
  │       ◄── exec_longjmp() ══════════════════════════╝
  │       ├── CLI
  │       ├── 一時スタック切替 → v86_mem_teardown() → 復元
  │       ├── TSS ESP0復元
  │       ├── v86_active = 0
  │       ├── STI
  │       ├── シリアルポート再初期化
  │       ├── デバッグダンプ
  │       ├── v86_disk_clear() → loop_devデタッチ
  │       ├── v86_restore_screen()
  │       ├── TVRAM復元
  │       └── FM音源/EGC状態復元
  │
  └─ (10) リソース解放
          v86_vsync_cleanup()
          vfs_close(fd)
          v86_timeout_ticks = 6000 (デフォルトに復帰)
          v86_native_mode = 0
```

## 8.4 終了シーケンス

### longjmpでの復帰

GPハンドラが `return 1` を返すと、`isr_stub.asm` が `exec_longjmp()` を呼び、
`v86_session_run_core()` の `exec_setjmp()` 直後に制御が戻る。

### teardown時の一時スタック

longjmpで戻った直後は:
- ページテーブルがまだV86バッキングRAMを指している
- カーネルスタックの物理ページがバッキングRAMに上書きされている

このため、`v86_kstack` (64KB静的配列) に一時的にスタックを切り替えて
`v86_mem_teardown()` を実行し、ページテーブルを元に戻す。

```asm
mov esi, esp          ; 現在のESP保存
mov edi, ebp          ; 現在のEBP保存
mov esp, tmp_stack    ; 一時スタックに切替
call v86_mem_teardown ; ページテーブル復元
mov esp, esi          ; 元のスタックに復帰 (実体が復活)
mov ebp, edi
```

## 8.5 終了理由

| enum値 | トリガー | 動作 |
|--------|---------|------|
| V86_EXIT_TRAP_PORT | OUT 0xFE | VDOSQUIT.COM が発行 |
| V86_EXIT_DOS_TERM | INT 20h / INT 21h AH=4Ch | DOSプログラム終了 (非ネイティブモード) |
| V86_EXIT_REBOOT | OUT 0xF0 | CPUリセットポート書き込み |
| V86_EXIT_BIOS_ROM | CS >= 0xF000 | リセットベクタ到達 |
| V86_EXIT_TIMEOUT | tick_count >= deadline | タイムアウト (ネイティブモードでは無効) |
| V86_EXIT_UNKNOWN_OP | 不明なオペコード | デバッグ用 |
| V86_EXIT_HOTKEY | Ctrl+GRPH+DEL / STOP | 強制脱出 |

## 8.6 Auto-Typer

### 動作原理

100Hz (IRQ0) ごとに `v86_session_on_tick()` が呼ばれ、
BDAキーボードバッファに文字を1つずつ注入する。

```
初期ディレイ: V86_AUTO_TYPE_DELAY = 400 tick (約4秒)
注入間隔:    V86_AUTO_TYPE_INTERVAL = 5 tick (50ms)
```

### バッファ操作

BDA規約に従い、**TAILポインタを進める** (生産側):
```
1. tail = BDA[KB_TAIL]
2. BDA[tail] = ASCII文字
3. BDA[tail+1] = スキャンコード
4. tail += 2 (0x522で0x502にラップ)
5. BDA[KB_TAIL] = tail
6. BDA[KB_COUNT]++
```

### 使用例

```
vdos /host/msdos5.fdi "A:\\IO.SYS\r"
```
→ DOS起動後に自動的に "A:\IO.SYS" + Enter を入力

## 8.7 強制脱出ホットキー

100Hzポーリングで物理キー押下状態を確認:

1. **Ctrl + GRPH + DEL**: PC-98のリセット相当 (GRPH = PC/AT の Alt)
2. **STOP キー**: PC-98固有キー。DOSでは通常未使用

`kbd_is_pressed()` は物理キースキャンバッファを直接参照するため、
DOSがキーバッファを消費しても検出可能。

## 8.8 デバイス状態退避・復元

### FM音源 (OPN)

**退避**: V86開始前に 0x188 (OPNアドレス), 0x18A (OPN2アドレス) を読み取り

**V86開始時リセット**:
1. SSGレジスタ 00h-0Dh 全クリア
2. SSGミキサー (reg 07h) = 0xBF (I/Oポート入力、全Tone/Noise OFF)
3. FM全チャンネル Key-OFF (reg 28h: ch0/1/2)
4. タイマー停止+フラグリセット (reg 27h = 0x30)

**V86終了時復元**:
1. FM全チャンネル Key-OFF (サイレンス化)
2. OPNアドレスラッチ復元

### EGC

**退避**: 0x04A0-0x04AE (8ワード) を保存
**復元**: V86終了時に保存値を書き戻し
