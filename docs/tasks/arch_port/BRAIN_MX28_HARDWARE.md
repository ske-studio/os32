# SHARP Brain (i.MX28 世代) ハードウェア調査

*調査日: 2026-09-08 / 種別: **調査記録** (計画でも仕様でもない)*
*主対象: **PW-SH4** (実機を保有) / 対象範囲: PW-SH1〜SH7 とその同世代機*

*他アーキテクチャ移植調査の 1 件。共通の調査軸と対象一覧は [00_INDEX.md](00_INDEX.md) を見ること。*

この文書は **一次情報の書き写しと、そこから読める OS32 側の差分** だけを書く。
実機での実測はまだ 1 つも含まれない (§9 参照)。断定できない項目は §10「未確認事項」に集めた。

当初は PW-SH5 を対象に調べたが、**実機として PW-SH4 を入手した**ため主対象を SH4 に移した。
両者は後述のとおりキーマップ以外ほぼ同一であり、世代で共通する事実を本文に、機種差だけを §4-0 の表に置く。

---

## 1. 結論 (先に要点)

| 問い | 答え |
|---|---|
| PW-SH4 の中身は何か | Freescale/NXP **i.MX28 系 SoC** (ARM926EJ-S / ARMv5TEJ)、DRAM 128MB、854×480 の MPU 接続 LCD |
| SH5 との違いは | **キーマトリクスの配列だけ**。U-Boot のボード対応はターゲット名と DTB 名以外バイト単位で同一 |
| OS32 はそのまま動くか | **動かない**。OS32 は i386 専用 (保護モード / GDT / IDT / PIC / V86 / `int 0x80`)。命令セットが違う |
| どれくらいの作業か | 「移植」ではなく **ARM 版 OS32 の新規実装**。カーネル中枢・全ドライバ・ブート・KAPI ABI・全バイナリ資産が作り直し |
| 逆に流用できるものは何か | C89 で書かれた **FS (ext2/FAT/VFS)、SQLite 統合、シェル、libos32* の大半、userland のロジック** |
| 独自コードは載せられるか | **載る**。BrainLILO が ED-SH4 を `gen3_4.bin` として対応済みで、SD カードだけで完結し **リセットボタンで純正 WinCE に戻る** |
| 一番大きな技術的壁は何か | ① ISA が違うこと ② **NP21/W に相当する検証環境が無い** ③ V86/MS-DOS 機能は原理的に消滅 |

先行事例として **brain-hackers / Brainux** が同じ機種に U-Boot + Linux を通しており、
PW-Sx4 世代は「Linux 起動 ✅ / キーボード ✅」として対応表に載っている。
本調査のハードウェア情報はほぼすべて、その公開ソース (U-Boot ボード対応、Linux DT、BrainLILO、Wiki) から取った。
**PW-SH4 で独自コードを起動する経路は既に他人が拓いている**のが唯一かつ最大の追い風である。

---

## 2. PW-SH4 の公表仕様

| 項目 | 値 |
|---|---|
| 型番 (内部) | **ED-SH4** (BrainLILO のモデル表記) |
| 発売 | **2017 年 1 月 19 日**、高校生向けモデル (170 コンテンツ) |
| 画面 | 5.5 型高精細カラー液晶、タッチパネル付き |
| 重量 | 約 290g |
| 駆動時間 | 約 100 時間 |
| 純正 OS | Windows Embedded CE 6.0 |

公表仕様に **CPU 型番・RAM 容量の記載は無い**。以下は brain-hackers の実装から読み取った値である。

---

## 3. SoC — i.MX28 (ARM926EJ-S)

### 3-1. コア

| 項目 | 値 | 出典 |
|---|---|---|
| コア | ARM926EJ-S (**ARMv5TEJ**) | linux-brain `imx28.dtsi` の `compatible = "arm,arm926ej-s"` |
| クロック | U-Boot が **CPU 分周比 1 = 480MHz** に設定 (`mxs_set_divcpu(1)`、PLL 480MHz) | u-boot-brain `board/sharp/pwsh4/pwsh4.c` |
| キャッシュ | 16KB I / 16KB D、**VIVT** (仮想アドレスタグ) | ARM926EJ-S 仕様 |
| MMU | あり (ARMv5 の 2 段ページテーブル、1MB セクション / 4KB スモールページ) | 同上 |
| FPU | **無し** (i.MX28 に VFP 非搭載)。整数のみ | i.MX28 データシート |
| エンディアン | リトルエンディアン (x86 と同じ) | — |
| 非整列アクセス | **不可**。ARMv5 は非整列 load/store が壊れる (回転した値が返る) | ARMv5 アーキテクチャ仕様 |
| クロック制御 | Linux 側に cpufreq 実装なし (2021 年時点) | Brainux Wiki ロードマップ |

正確な派生型番 (i.MX283 / i.MX287 など) は公開情報で確認できなかった。
brain-hackers 側も `ARCH_MX28` として汎用に扱っており、機種差はボードファイルで吸収している。
なお NXP の定格は 454MHz であり、480MHz 設定は定格超えである点に注意 (§10)。

### 3-2. 物理メモリ配置

| 範囲 | 内容 | 出典 |
|---|---|---|
| `0x00000000–` | オンチップ SRAM (128KB)。ブート ROM が SPL をここに展開する | U-Boot `CONFIG_SPL_TEXT_BASE=0x00001000` |
| `0x40000000–0x47FFFFFF` | **DRAM 128MB** | `imx28-brain.dtsi` `memory@40000000 reg = <0x40000000 0x08000000>`、U-Boot `PHYS_SDRAM_1_SIZE 0x8000000` |
| `0x40200000` | U-Boot 本体のロード先 | `CONFIG_SYS_TEXT_BASE` |
| `0x41000000` / `0x42000000` | U-Boot の DT / カーネルロード先 | `brain_mx28_common.h` |
| `0x80000000–0x8003FFFF` | APBH バス (高速周辺 + DMA) | `imx28.dtsi` |
| `0x80040000–0x8007FFFF` | APBX バス (低速周辺) | 同上 |
| `0x80080000–` | AHB (USB) | 同上 |

OS32 の `include/memmap.h` が前提にしている **1MB (0x100000) 起点のカーネル帯**、`0xA8000` の VRAM、
`0xF0000` の BIOS ROM は **すべて存在しない**。番地体系は全面的に書き直しになる。

### 3-3. 周辺 I/O の番地 (i.MX28 共通)

| 番地 | ブロック | OS32 での相当物 |
|---|---|---|
| `0x80000000` | **ICOLL** 割り込みコントローラ | i8259A PIC (`kernel/idt.c`, `isr_stub.asm`) |
| `0x80004000` / `0x80024000` | DMA-APBH / DMA-APBX (ディスクリプタ方式) | PC-98 DMAC ([HW2] の 64KB 境界制約は無関係になる) |
| `0x8000C000` | GPMI (NAND) | — |
| `0x80010000` | **SSP0** = 内蔵 eMMC (8bit) | IDE (`drivers/ide.c`) |
| `0x80012000` | **SSP1** = microSD (4bit) | FDC / IDE |
| `0x80014000` | SSP2 (SPI。副画面のある機種用。**SH1〜SH7 では未使用**) | — |
| `0x80018000` | pinctrl + **GPIO 0〜4** (バンクあたり 32 本) | — (PC-98 に相当なし) |
| `0x8001C000` | DIGCTL | — |
| `0x8002C000` | OCOTP (ヒューズ、ブート設定) | — |
| `0x80028000` | DCP (暗号 + memcpy アクセラレータ) | — |
| `0x8002A000` | PXP (2D 変換・回転・色変換エンジン) | GDC/EGC 相当 ([HW1] で禁じている類の機能) |
| `0x80030000` | **LCDIF** 表示コントローラ | GFX バックエンド (`gfx/backend_*.c`) |
| `0x80040000` | CLKCTRL (PLL / 分周) | — |
| `0x80042000` / `0x80046000` | SAIF0 / SAIF1 (I2S) | FM 音源 (`drivers/fm.c`) |
| `0x80044000` | POWER (内蔵 PMU、電源断) | — |
| `0x80050000` | **LRADC** (低速 ADC + 4 線式タッチパネル) | マウス (`drivers/mouse*.c`) |
| `0x80056000` | RTC | `drivers/rtc.c` |
| `0x80058000` / `0x8005A000` | I2C0 / I2C1 | — |
| `0x80064000` | PWM (バックライト・ブザー) | — |
| `0x80068000` | **TIMROT** タイマ | i8253 PIT |
| `0x80074000` | **DUART** (デバッグシリアル) | `drivers/serial.c` (8251) |
| `0x8007C000` / `0x8007E000` | USB PHY 0/1 | — |
| `0x80080000` | USB0 (ホスト / 切替でデバイス) | — |

---

## 4. 実装デバイス

`imx28-brain.dtsi` (Brain 共通) と `imx28-pwsh4.dts` (機種固有) から読める構成。

### 4-0. 機種差分 (PW-SH1〜SH7)

DT を機械的に比較した結果。**世代差は驚くほど小さい**。

| 機種 | 画面 | キーボード | Symbol キー位置 | タッチ | DT 名 |
|---|---|---|---|---|---|
| PW-Sx1, HC4, SR1 | 800×480 (112×67mm) | **I2C** (`sharp,brain-kbd-i2c` @0x28) | — | LRADC | `imx28-pwsh1` |
| PW-Sx2, HC5 | 800×480 | I2C | — | LRADC | `imx28-pwsh2` |
| PW-Sx3, HC6 | 854×480 (121×68mm) | GPIO マトリクス | `(4,3)` | LRADC | `imx28-pwsh3` |
| **PW-Sx4, H7700, SR2** | **854×480 (121×68mm)** | **GPIO マトリクス** | **`(4,3)`** | **LRADC** | **`imx28-pwsh4`** |
| PW-Sx5, H7800, AA1, AJ1 | 854×480 | GPIO マトリクス | `(4,6)` | LRADC | `imx28-pwsh5` |
| PW-Sx6, H8000, AA2, AJ2 | 854×480 | GPIO マトリクス | `(2,4)` | LRADC | `imx28-pwsh6` |
| PW-Sx7, H8100/H9100, SR3 | 854×480 | GPIO マトリクス | `(2,4)` | LRADC | `imx28-pwsh7` |

- **SH3 と SH4 の DT 差分はキーマップのラベルと位置だけ**、**SH4 と SH5 の差分もキーマップだけ**。
  LCD の解像度・寸法・MADCTL フラグ・有効化 GPIO、キーマトリクスのピン、タッチ、MMC、音声はすべて同一。
- **U-Boot のボード対応 (`configs/pwsh4_defconfig` と `board/sharp/pwsh4/pwsh4.c`) は SH5 版と、
  ターゲット名と DTB ファイル名以外まったく同一**。クロック設定・MMC 初期化・LCD 設定まで一致する。
- `semtech,sx8650` (外付けタッチ IC) と SPI 副画面 (`brain,st7586`) は **SH1〜SH7 のどれも使っていない**。

つまり **SH4 向けに調べた低レベル情報は SH3/SH5/SH6/SH7 にもほぼそのまま通る**。
移植を始めるにあたって機種を選び直す理由は薄い。

### 4-1. 表示 — ここが最重要

| 項目 | 値 |
|---|---|
| パネル駆動 IC | **Ilitek ILI9805** |
| 接続 | LCDIF の **16bit MPU (system) モード** — RS/CS/WR/RD の 8080 系。RGB ドットクロック方式ではない |
| 画素形式 | **RGB565** (`0x3a = 0x55`、`MEDIA_BUS_FMT_RGB565_1X16`) |
| 論理解像度 | 854×480 (物理は 480×854 縦長。`MADCTL` の MV=転置 + GS で横向きにしている) |
| 表示寸法 | 121 × 68 mm |
| 有効化 GPIO | `gpio0[26]`, `gpio0[27]`, `gpio4[16]` を H にしてから 20ms 待つ |
| TE (ティアリング) | `LCD_RESET` パッドを `LCD_VSYNC` に流用し、LCDIF の VSYNC モードで受ける |
| バックライト | PWM0 + `gpio3[5]` で enable。8 段階 |

**転送モデル**が OS32 の描画方式と根本的に違う。
PC-98 は VRAM への直接 CPU 書き込み ([HW1]) だが、Brain は「DRAM 上のフレームバッファを LCDIF の DMA でパネルへ流し込む」方式で、
Linux ドライバの present 相当は次の 4 レジスタ書き込みだけである (`drivers/gpu/drm/tiny/brain.c`):

```
CUR_BUF  = フレームバッファ物理アドレス
NEXT_BUF = 同上
TRANSFER_COUNT = VCOUNT(480) | HCOUNT(854)
CTRL_SET = DATA_SELECT | RUN
```

- 1 フレーム = 854 × 480 × 2 = **819,840 バイト (約 800KB)**。DRAM 128MB に対しては軽い。
- CPU は転送に関与しない。**OS32 の `gfx_present()` はむしろ安く実装できる**。
- ただし Linux 実装は `drm_atomic_helper_damage_merged()` で damage 矩形を求めておきながら、
  **その矩形を使わず毎回全画面を転送している** (`brain_fb_dirty_full`)。
  OS32 の InvalidateRect 方式 (`docs/tasks/gui/DESIGN.md`) を活かすなら、
  ILI9805 の Column/Page Address Set で転送範囲を絞る余地がここにある。
- 代償として **キャッシュ管理が必須**。ARM926 は D キャッシュが VIVT でハードウェアコヒーレンシが無いため、
  描画後・DMA 前に clean が要る。OS32 の GFX は x86 のコヒーレントキャッシュを暗黙に前提にしている。
- planar 16 色 / PEGC PACKED8 を前提にした `gfx/backend_pc98.c` `backend_pegc.c` は使えない。
  RGB565 バックエンドの新規追加になる (HAL の枠組み自体は既存設計に乗る)。

### 4-2. キーボード (PW-SH4)

`imx28-pwsh4.dts` の `sharp,brain-kbd-gpio`。**GPIO マトリクスの手動スキャン**であり、割り込みは無い。

| 項目 | 値 |
|---|---|
| 入力 (行) | `gpio4[0]`〜`gpio4[7]` の 8 本 |
| 出力 (列) | `gpio2[16]`〜`gpio2[21]` + `gpio4[8]` の 7 本 |
| Symbol キー | **`(4,3)`**。押下中は第 2 キーマップ (数字・記号) に切り替わる **ソフト実装** |

Brainux が SH4 世代に割り当てている修飾キーは以下 (キートップの刻印 → PC キーボード):

| 修飾キー | PW-SH4 のキー |
|---|---|
| Shift | シフト |
| Ctrl | **音声** |
| Alt | **ページアップ** |
| 記号 (第2面) | 記号 |

SH5 では Ctrl と Alt が入れ替わる (Ctrl=ページアップ / Alt=音声)。**この差が SH4 と SH5 の実質的な唯一の違い**である。

OS32 側の `drivers/kbd.c` (PC-98 の 8251 + キーボード I/F、IRQ 駆動) は完全に別物になる。
**タイマ割り込みからのポーリング走査 + チャタリング除去**を自前で書く必要がある。

### 4-3. タッチパネル

`lradc@80050000` を `fsl,lradc-touchscreen-wires = <4>` で使う **4 線式抵抗膜**。
SoC 内蔵 ADC で読むため外付けコントローラは無い。

- 分解能は **12bit (0〜4095)**。
- Brainux Wiki の実測キャリブレーション例: `min_x=147, max_x=3618, min_y=3826, max_y=350`。
  **Y 軸が反転している** (min > max) 点に注意。
- 座標は生の ADC 値であり、**キャリブレーションは OS 側の責任**。
- OS32 のシームレス絶対座標マウス (`drivers/mouse_seamless.c`) の考え方は流用できるが、実装は別。

### 4-4. ストレージ

| 経路 | デバイス | 備考 |
|---|---|---|
| SSP0 (8bit) | **内蔵 eMMC** | 純正 WinCE と辞書コンテンツが入っている |
| SSP1 (4bit) | **microSD スロット** | カード検出・WP は常に「挿入・書込可」を返す実装 (U-Boot `brain_mmc_cd`) |
| GPMI | NAND | Brain では未使用の模様 |

Brainux は **SD カード上に全システムを置く** (第1パーティション = `boot` (FAT)、第2 = root)。
OS32 の FS 層 (VFS / ext2 / FatFs / ISO9660) は **C89 で書かれておりブロックデバイスの下だけ差し替えれば流用できる**。
ここが移植で最も分のいい部分である。必要なのは「SSP/MMC ドライバ 1 本」。
ただし `fs/` は x86 の非整列アクセス許容に依存している箇所がある可能性が高く、監査が要る (§8-3)。

### 4-5. 音声

- SAIF0 / SAIF1 (I2S) + **SGTL5000 コーデック** (I2C0 アドレス `0x0a`)。
- ブザーは PWM4 + `gpio3[26]` の枠が dtsi にあるが、**SH1〜SH7 では有効化されていない**。
- OS32 の FM 音源ドライバ (`drivers/fm.c`, `kernel/snd_engine.c`) は使えない。PCM ストリーム方式への作り直し。

### 4-6. USB — 検証経路として重要

| 項目 | 値 |
|---|---|
| 既定 | `dr_mode = "host"`。VBUS は `gpio3[9]` |
| 電源 | **Brain 自身は VBUS (5V) を供給できない**。ホストで使うには外部電源付き OTG ケーブルが要る |
| デバイス化 | DT の `usb@80080000` を `dr_mode = "peripheral"` に書き換えると **USB ガジェットになる** |

Brainux はこれを使って **USB ケーブル 1 本で PC と双方向通信 (SSH / インターネット共有)** を実現している。
OS32 に置き換えると、**USB CDC ガジェットが `/api/cmd` 相当の遠隔試験経路になりうる**唯一の現実的な候補である (§8-1)。

### 4-7. その他

| デバイス | 状態 |
|---|---|
| RTC | dtsi で **`status = "disabled"`**。Brain では使われていない (要調査) |
| 電源コントローラ | Linux 側 **未対応** (2021 年時点)。充電状態・電池残量は読めない |
| 電源断 | POWER ブロックの `poweroff`。電源ボタンは PC の電源ボタンと同じ扱い |
| 有線/無線 LAN | **本体には無し**。dtsi の `reg_fec_3v3` 等は i.MX28 EVK からの残骸。Wi-Fi は SDIO / USB ドングルで検証中の段階 |

ネットワークが無いため、`docs/tasks/network/` の LGY-98 / NE2000 系はすべて対象外になる。

---

## 5. 独自コードを起動する経路 (PW-SH4)

**PW-SH4 は対応済み**である。BrainLILO のモデル表に `{L"ED-SH4", L"gen3_4.bin"}` として載っており、
Brainux の対応機種表でも「PW-Sx4, H7700, SR2 → Linux 起動 ✅ / キーボード ✅」となっている。

| 経路 | 仕組み | 危険度 |
|---|---|---|
| **① アプリメニューから起動** | SD ルートに `アプリ/BrainLILO/` (`AppMain.exe` + 空の `index.din`) と `LOADER/u-boot.bin` を置き、純正 WinCE のアプリメニューから起動する | **低**。eMMC を一切書かない |
| **② SD からの直接起動** | SD 第1パーティションの `nk` ディレクトリの中身をルートへコピーし、WinCE の起動シーケンスに割り込む | **低**〜中。SD 上で完結する |
| **③ eMMC へインストール** | 純正領域を上書きする | **高**。Wiki では 2021 年時点「研究中」、2023 年に達成報告あり |

**復旧手段が確認できたのが大きい**: ①② はいずれも SD カード上で完結し、
**リセットボタンを押せば純正 Windows CE が起動する**。SD を抜けば元の電子辞書に戻る。
つまり **③ に手を出さない限り文鎮化しない**。当面 ③ は対象外にしてよい。

### 5-1. 注意点

- **SH4 世代から「非公式アプリ起動のプロテクト」が入った** (PW-SB 系を除く)。
  後に回避ツールが作られており BrainLILO は SH4 に対応しているが、
  **具体的に何をどう回避しているのかは未確認** (§10)。SH3 以前より手数が増えるのは確か。
- 正常にシャットダウンせずリセットすると **SD カード上のデータが壊れることがある**。
- ビルド環境は Debian/Ubuntu + `gcc-arm-linux-gnueabi`。
  `buildbrain` リポジトリで `make udefconfig-sh4` → `make ubuild` (`u-boot.sb` 生成) → `make nk.bin`。

### 5-2. OS32 から見た起動物の形

U-Boot が入る前提に立てるなら、OS32 側の起動物は **U-Boot が `bootz` で読める形式** (ロードアドレス `0x42000000`、
DT は `0x41000000`) にするのが最短で、`boot/` の IPL / ローダ (PC-98 の INT 1Bh、`.8086` 制約、LBA 2–17) は
**全部不要になる**。SD の第1パーティション (FAT) にカーネルを置き、U-Boot の `fatload` + `bootz` で起動する。

---

## 6. OS32 側の棚卸し — 何が残り、何が消えるか

現在のソース規模と、Brain での扱い。

| 領域 | 規模 (行) | Brain での扱い |
|---|---|---|
| `boot/` | 2,684 | **全廃**。U-Boot に置き換え |
| `kernel/` | 13,592 | 中枢は**作り直し**。`gdt/idt/tss/paging/pgalloc/isr_stub/kentry/ring3_entry/setjmp` は x86 専用。`v86` 系 15 ファイル (C 6 + ヘッダ 6 + asm 3) は **ARM に等価物が無く消滅** |
| `exec/` | 1,459 | ロジックは流用可。ページテーブル操作・CPL 遷移・`int 0x80` は書き直し |
| `drivers/` | 8,948 | **全廃**。IDE / FDC / ATAPI / 8251 / PC-98 キーボード / バスマウス / FM / NE2000 / LGY-98 / NP2SysP はどれも存在しないハード |
| `gfx/` | 2,588 | バックエンド 3 種は全廃。`gfx_core.c` の描画ロジックは RGB565 化して流用可 |
| `fs/` | 14,555 | **ほぼそのまま流用可** (要: 非整列アクセス監査 + ブロック層の差し替え) |
| `lib/` | 291,974 (大半は SQLite) | SQLite は移植性が高い。`kstring_asm.asm` のみ書き直し |
| `kapi/` | 2,027 | **ABI を再定義**。`int 0x80` → `svc`、System V i386 ABI / `__cdecl` → AAPCS。`sdk/kapi.json` の枠組みと生成器は流用できる |
| `userland/` | 35,561 | ソースは大半流用可。ただし **全バイナリの再ビルドが必要**、OS32X フォーマットと `crt0.asm` は作り直し |
| `apps/` `game/` (submodule) | — | 同上。静的リンクのため全滅 → 全再ビルド |

アセンブリは `.asm` 20 ファイル + インラインアセンブリ 12 ファイル。数としては多くないが、
**そこにあるものがほぼ全部「OS の中枢」**である点が問題で、行数は作業量を表さない。

---

## 7. 移植を阻む論点 (重い順)

### 7-1. 検証環境が無い

OS32 の開発サイクルは NP21/W ai-debug フォーク (HTTP デバッグサーバ + MCP: レジスタ・メモリ・逆アセンブル・
ブレークポイント・トレース) に完全に依存している。`make hotdeploy` も `/api/cmd` も `emu_read_mem` も Brain には無い。

- i.MX28 を実用的な精度で再現するエミュレータは知られていない (QEMU に mxs マシンは無い)。
- **Brainux Wiki にシリアルコンソールの記述が一切無い**。コミュニティは分解せず SD と本体画面だけで開発している。
  DUART は SoC にあるが、基板上でどう引き出すかは誰も文書化していない (§10)。
- 代わりに現実的な経路は 2 つある:
  1. **U-Boot / カーネルのコンソールを LCD に出す**。U-Boot の Brain 設定は既に `stdout=serial,vga` で、
     LCD 出力を持っている。半田付けなしで最初のログが見える。
  2. **USB ガジェット** (§4-6)。`dr_mode=peripheral` で PC と USB 1 本。
     OS32 に CDC ガジェットを実装できれば `/api/cmd` 相当の遠隔試験が復活する。
- それでも **開発速度が 1〜2 桁落ちる**という評価は変わらない。これが最大の障害である。

### 7-2. V86 サブシステムの消滅

`kernel/v86*` 一式 (C 6 + ヘッダ 6 + asm 3) と MS-DOS ゲスト実行は **x86 の仮想 8086 モードそのもの**であり、
ARM には対応する機構が無い。エミュレータを書く以外に道は無く、それは別プロジェクトである。
`docs/tasks/v86v2/` の成果は Brain では回収できない。

### 7-3. 非整列アクセスとキャッシュ

x86 では黙って通っていたコードが ARMv5 では壊れる。具体的に危ないのは:

- ext2 / FAT / ISO9660 / MGX / OS32X ヘッダなど、**バイト列に構造体ポインタを重ねている箇所**。
- `kstring` / `kmalloc` のアラインメント前提。
- GFX の 32bit 単位コピー最適化。
- DMA (LCDIF・MMC) と CPU キャッシュのコヒーレンシ。OS32 には現在キャッシュ管理の概念が無い。

移植の前段として **既存 C コードの非整列アクセス監査**が要る。これは実機が無くても今日できる作業であり、
**2026-09-08 に実施した** → [M0_PORTABILITY_AUDIT.md](M0_PORTABILITY_AUDIT.md)。
結果は「想定より軽い」: 確定的な不具合は `lib/utf8.c` と `kapi/kapi_db.c` の 2 件で、
構造体レイアウトは i386 と ARM で完全一致していた。重いのはむしろキャッシュ側である。

### 7-4. ABI と既存バイナリ資産

`sdk/kapi.json` を正典とする KAPI の**仕組み**は残せるが、中身の規約 ([C3] の `__cdecl`、System V i386 ABI、
`int 0x80` スロット) はすべて AAPCS + `svc` に置き換わる。`KAPI_VERSION` の連番は引き継げず、
**別 ABI として切る**判断になる。既にビルド済みの `.bin`、`libos32gui.shlib`、
submodule のアプリ・ゲームは 1 つも動かない。

### 7-5. 制約規則の再定義

`docs/CONSTRAINTS.md` の [HW1] (EGC/GRCG/GDC 禁止・VRAM 直書き)、[HW2] (DMA 64KB 境界)、
[V1]〜[V3] (HostDrv / NHD / curl タイムアウト) は **PC-98 と NP21/W に固有**であり、
Brain では意味を失うか、別の規則 (キャッシュ管理必須、非整列アクセス禁止) に置き換わる。
[C1]〜[C4] (C89 / kstring / 三層定数) はそのまま維持できる。

---

## 8. もし進めるなら — 段階案 (提案であり計画ではない)

各段は「結果が画面またはホスト PC から確認できること」を完了条件にする。

| 段 | 内容 | 実機の要否 |
|---|---|---|
| M0 | 既存 C コードの**非整列アクセス / キャッシュ前提の監査**、ARM クロスコンパイラの整備 → **実施済み (2026-09-08)**: [M0_PORTABILITY_AUDIT.md](M0_PORTABILITY_AUDIT.md) | 不要 |
| M1 | **Brainux の SD イメージをそのまま起動**して実機とハードウェアの挙動を確認 (キーマップ、タッチ、画面) | 要 |
| M2 | `buildbrain` で **PW-SH4 用 U-Boot を自前ビルド**し、SD から起動させる | 要 |
| M3 | U-Boot から `bootz` で読める最小カーネル。**LCD コンソールに "hello"** | 要 |
| M4 | ICOLL + TIMROT で**割り込みとタイマ**、`sys_get_tick` 相当 | 要 |
| M5 | MMU + ページング、kmalloc、kselftest の ARM 版 | 要 |
| M6 | **LCDIF + ILI9805 で画面**。RGB565 バックエンド、KCG フォント描画、キャッシュ clean | 要 |
| M7 | GPIO マトリクスキーボード (SH4 配列)、LRADC タッチ | 要 |
| M8 | SSP/MMC ドライバ → 既存 VFS/ext2 の接続 | 要 |
| M9 | USB CDC ガジェットで遠隔試験経路 (`/api/cmd` 相当) を復活させる | 要 |
| M10 | KAPI ABI (AAPCS + `svc`) 再定義、`crt0` と実行フォーマット、userland 再ビルド | 要 |

M0 だけは今のリポジトリで単独に価値がある (x86 でも潜在バグの発見になる)。
M1・M2 は OS32 のコードを 1 行も書かずに実機とツールチェインの健全性を確かめられるので、着手するなら最初にここを通す。

---

## 9. 実機で確かめる価値のあること (PW-SH4 入手済み)

実機があるので、以下は **調査として今すぐ埋められる**。いずれも OS32 のコードを書く前にできる。

1. Brainux の SD イメージを焼いて起動し、**リセットで純正 WinCE に戻ることを自分で確認する** (§5 の前提)。
2. キーマトリクスの実測 — SH4 の Symbol キー `(4,3)` と修飾キー割当が DT どおりか。
3. LRADC タッチのキャリブレーション値を自分の個体で採る (Wiki の値は他人の個体)。
4. **LCD の実測フレームレート** — 全画面 800KB 転送に何 ms かかるか。OS32 の GUI 性能見積りの根拠になる。
5. `/proc/cpuinfo`・`dmesg` から **SoC の派生型番と実クロック**を読む (§10-1 の解決)。
6. eMMC と DRAM の実容量 (§10-2, §10-3 の解決)。
7. USB を `peripheral` に切り替え、**USB ガジェット経路が本当に使えるか**を確かめる (§7-1 の代替経路の可否)。
8. 基板を開けずに DUART が引き出せるか (§10-4)。

---

## 10. 未確認事項

実機または一次資料でしか埋まらない項目。§9 で埋まるものは番号を対応させた。

1. **i.MX28 の正確な派生型番** (i.MX283 / i.MX287 など)。定格 454MHz に対し U-Boot が 480MHz を設定している理由も含めて未確認。
2. DRAM 128MB は DT と U-Boot の記述であり、**実チップの容量は未確認**。
3. 内蔵 eMMC の容量。
4. **DUART パッドの物理位置**、分解の要否、信号電圧。Brainux Wiki に記述が無く、コミュニティも使っていない模様。
5. **SH4 世代のプロテクトの具体的内容**と、BrainLILO がそれをどう回避しているか。
6. ブートモードピン / OCOTP による SD 直接ブートの可否と、設定を戻せるか。
7. RTC が dtsi で無効な理由 (存在しない / 未対応 / 電池が別)。
8. ブザー・音声出力の実装有無 (dtsi の枠は SH1〜SH7 で有効化されていない)。
9. 電源管理 (充電、サスペンド、電池残量読み出し) の方法。Linux 側は未対応。
10. LCD の実測フレームレートと LCDIF 転送に要する時間。
11. eMMC を書き換えた場合の復旧手段 (経路 ③ に進む場合のみ必要)。

---

## 11. 出典

一次情報 (コードを直接確認):

- brain-hackers / **u-boot-brain** — `configs/pwsh4_defconfig`, `board/sharp/pwsh4/{pwsh4.c,README}`, `board/sharp/common/{lcd.c,lcd.h,Kconfig}`, `include/configs/brain_mx28_common.h` : https://github.com/brain-hackers/u-boot-brain
- brain-hackers / **linux-brain** — `arch/arm/boot/dts/{imx28-pwsh1..7.dts,imx28-brain.dtsi,imx28.dtsi}`, `drivers/gpu/drm/tiny/brain.c`, `drivers/input/keyboard/brain-kbd-gpio.c` : https://github.com/brain-hackers/linux-brain
- brain-hackers / **brainlilo** — `README.md`, `models.h` (ED-SH4 → `gen3_4.bin`) : https://github.com/brain-hackers/brainlilo
- brain-hackers / **wiki.brainux.org** — `collections/_beginners/get-started.md` (対応機種表・キー割当・起動手順), `_beginners/roadmap.md`, `_tips/{touch-panel,usb-ethernet-gadget,otg}.md`, `_build/uboot.md` : https://github.com/brain-hackers/wiki.brainux.org
- brain-hackers / **buildbrain** (SD イメージ配布・ビルド) : https://github.com/brain-hackers/buildbrain

二次情報:

- 価格.com PW-SH4 : https://kakaku.com/item/J0000022400/
- Brainux : https://brainux.org/ , https://wiki.brainux.org/
- 「詳解・電子辞書で Linux がブートするまで」 (Takumi Sueda) : https://speakerdeck.com/puhitaku/boot-linux-on-sharp-brain-explained
- NXP i.MX28 データシート (IMX28CEC) : https://www.nxp.com/docs/en/data-sheet/IMX28CEC.pdf
- OUCC「SHARP Brain 用アプリケーションの作成方法」 : https://oucc.org/blog/articles/303/

本調査で参照した外部リポジトリは一時作業領域にクローンしたのみで、本リポジトリには一切取り込んでいない。
