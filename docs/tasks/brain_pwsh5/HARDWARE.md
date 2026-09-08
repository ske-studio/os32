# SHARP Brain PW-SH5 ハードウェア調査

*調査日: 2026-09-08 / 種別: **調査記録** (計画でも仕様でもない) / 対象: OS32 を PW-SH5 に載せる可能性の技術的評価*

この文書は **一次情報の書き写しと、そこから読める OS32 側の差分** だけを書く。
実機は未入手であり、実測値は 1 つも含まれない。断定できない項目は §9 の「未確認」に集めた。

---

## 1. 結論 (先に要点)

| 問い | 答え |
|---|---|
| PW-SH5 の中身は何か | Freescale/NXP **i.MX28 系 SoC** (ARM926EJ-S / ARMv5TEJ)、DRAM 128MB、854×480 の MPU 接続 LCD |
| OS32 はそのまま動くか | **動かない**。OS32 は i386 専用 (保護モード / GDT / IDT / PIC / V86 / int 0x80)。命令セットが違う |
| どれくらいの作業か | 「移植」ではなく **ARM 版 OS32 の新規実装**。カーネル中枢・全ドライバ・ブート・KAPI ABI・全バイナリ資産が作り直し |
| 逆に流用できるものは何か | C89 で書かれた **FS (ext2/FAT/VFS)、SQLite 統合、シェル、libos32* の大半、userland のロジック**。言語レベルでは移植可能 |
| 一番大きな技術的壁は何か | ① ISA が違うこと ② **実機以外に検証手段が無い** (NP21/W に相当するエミュレータが無い) ③ V86/MS-DOS 機能は原理的に消滅 |
| 一番大きな非技術的壁は何か | PW-SH4 以降は独自コード起動の敷居が上がっており、失敗すると **文鎮化のリスクがある実機 1 台だけが試験環境** になる |

先行事例として **brain-hackers / Brainux** が同じ機種に U-Boot + Linux を通している。
本調査のハードウェア情報はほぼすべて、その公開ソース (U-Boot ボード対応と Linux DT) から取った。
つまり **「PW-SH5 で独自 OS を起動する経路は既に他人が拓いている」** のが唯一の追い風である。

---

## 2. 機体の公表仕様 (シャープ / 価格.com)

| 項目 | 値 |
|---|---|
| 発売 | 2018 年 (高校生向けモデル) |
| 画面 | 5.5 型 WVGA カラー液晶 **854×480**、タッチパネル付き、360 度回転ヒンジ |
| 内蔵メモリ (ユーザー領域) | 約 500MB |
| 外部記憶 | microSD / microSDHC / **microSDXC (64GB まで)** |
| 電源 | 内蔵リチウムイオン電池 (連続表示 約 120 時間) |
| 外形 | 152.4 × 96.5 × 18.4 mm / 約 270g (電池・タッチペン込み) |
| 純正 OS | Windows Embedded CE 6.0 |

公表仕様に **CPU 型番・RAM 容量の記載は無い**。以下は brain-hackers の実装から読み取った値である。

---

## 3. SoC — i.MX28 (ARM926EJ-S)

### 3-1. コア

| 項目 | 値 | 出典 |
|---|---|---|
| コア | ARM926EJ-S (**ARMv5TEJ**) | linux-brain `imx28.dtsi` の `compatible = "arm,arm926ej-s"` |
| クロック | U-Boot が **CPU 分周比 1 = 480MHz** に設定 (`mxs_set_divcpu(1)`、PLL 480MHz) | u-boot-brain `board/sharp/pwsh5/pwsh5.c` |
| キャッシュ | 16KB I / 16KB D、**VIVT** (仮想アドレスタグ) | ARM926EJ-S 仕様 |
| MMU | あり (ARMv5 の 2 段ページテーブル、1MB セクション / 4KB スモールページ) | 同上 |
| FPU | **無し** (i.MX28 に VFP 非搭載)。整数のみ | i.MX28 データシート |
| エンディアン | リトルエンディアン (x86 と同じ) | — |
| 非整列アクセス | **不可**。ARMv5 は非整列 load/store が壊れる (回転した値が返る) | ARMv5 アーキテクチャ仕様 |

正確な派生型番 (i.MX283 / i.MX287 など) は公開情報で確認できなかった。
brain-hackers 側も `ARCH_MX28` として汎用に扱っており、機種差はボードファイル側で吸収している。
なお NXP の定格は 454MHz であり、480MHz 設定は定格超えである点に注意 (§9)。

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
| `0x80014000` | SSP2 (SPI。第2画面のあるモデル用、SH5 では無効) | — |
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
| `0x80080000` | USB0 (ホスト) | — |

---

## 4. PW-SH5 の実装デバイス

`imx28-brain.dtsi` (Brain 共通) と `imx28-pwsh5.dts` (機種固有) から読める構成。

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
PC-98 は VRAM への直接 CPU 書き込み ([HW1]) だが、PW-SH5 は「DRAM 上のフレームバッファを LCDIF の DMA でパネルへ流し込む」方式で、
Linux ドライバの present 相当は次の 4 レジスタ書き込みだけである (`drivers/gpu/drm/tiny/brain.c`):

```
CUR_BUF  = フレームバッファ物理アドレス
NEXT_BUF = 同上
TRANSFER_COUNT = VCOUNT(480) | HCOUNT(854)
CTRL_SET = DATA_SELECT | RUN
```

- 1 フレーム = 854 × 480 × 2 = **819,840 バイト (約 800KB)**。DRAM 128MB に対しては軽い。
- CPU は転送に関与しない。**OS32 の `gfx_present()` はむしろ安く実装できる**。
- 代償として **キャッシュ管理が必須**。ARM926 は D キャッシュが VIVT でハードウェアコヒーレンシが無いため、
  描画後・DMA 前に clean が要る。OS32 の GFX は x86 のコヒーレントキャッシュを暗黙に前提にしている。
- planar 16 色 / PEGC PACKED8 を前提にした `gfx/backend_pc98.c` `backend_pegc.c` は使えない。
  RGB565 バックエンドの新規追加になる (HAL の枠組み自体は `docs/tasks/gui/DESIGN.md` の設計に乗る)。

### 4-2. キーボード

`imx28-pwsh5.dts` の `sharp,brain-kbd-gpio`。**GPIO マトリクスの手動スキャン**であり、割り込みは無い。

| 項目 | 値 |
|---|---|
| 入力 (行) | `gpio4[0]`〜`gpio4[7]` の 8 本 |
| 出力 (列) | `gpio2[16]`〜`gpio2[21]` + `gpio4[8]` の 7 本 |
| 配列 | QWERTY 相当 + 辞書キー (国語/英和和英/マイ辞書/履歴)、方向キー、Shift、戻る、音声 |
| Symbol キー | `(4,6)`。押下中は第 2 キーマップ (数字・記号) に切り替わる **ソフト実装** |

OS32 側の `drivers/kbd.c` (PC-98 の 8251 + キーボード I/F、IRQ 駆動) は完全に別物になる。
**タイマ割り込みからのポーリング走査 + チャタリング除去**を自前で書く必要がある。

### 4-3. タッチパネル

`lradc@80050000` を `fsl,lradc-touchscreen-wires = <4>` で使う **4 線式抵抗膜**。
SoC 内蔵 ADC で読むため外付けコントローラは無い (別世代機にある `semtech,sx8650` は SH5 では無効)。
座標は生の ADC 値であり、**キャリブレーションは OS 側の責任**。
OS32 のシームレス絶対座標マウス (`drivers/mouse_seamless.c`) の考え方は流用できるが、実装は別。

### 4-4. ストレージ

| 経路 | デバイス | 備考 |
|---|---|---|
| SSP0 (8bit) | **内蔵 eMMC** | 純正 WinCE と辞書コンテンツが入っている。ユーザー領域 約 500MB は公表値 |
| SSP1 (4bit) | **microSD スロット** | カード検出・WP は常に「挿入・書込可」を返す実装 (U-Boot `brain_mmc_cd`) |
| GPMI | NAND | Brain では未使用の模様 |

OS32 の FS 層 (VFS / ext2 / FatFs / ISO9660) は **C89 で書かれておりブロックデバイスの下だけ差し替えれば流用できる**。
ここが移植で最も分のいい部分である。必要なのは「SSP/MMC ドライバ 1 本」。
ただし `fs/` は x86 の非整列アクセス許容に依存している箇所がある可能性が高く、監査が要る (§7-3)。

### 4-5. 音声

- SAIF0 / SAIF1 (I2S) + **SGTL5000 コーデック** (I2C0 アドレス `0x0a`)。
- ブザーは PWM4 + `gpio3[26]` の枠が dtsi にあるが、**SH5 では有効化されていない**。
- OS32 の FM 音源ドライバ (`drivers/fm.c`, `kernel/snd_engine.c`) は使えない。PCM ストリーム方式への作り直し。

### 4-6. シリアル (デバッグ経路)

`duart@80074000` を `AUART0_CTS`/`AUART0_RTS` パッドに割り当てて使う。
**これが唯一の低レベルデバッグ手段**になる。物理的な引き出し方 (基板上のパッド位置、分解の要否、電圧レベル) は未確認 (§9)。

OS32 の遠隔試験は `curl http://127.0.0.1:8025/api/cmd` = NP21/W 内蔵デバッグサーバに全面依存している ([V3])。
PW-SH5 には相当物が無いため、**シリアル越しの rshell を最初に立てる**のが実質的な前提条件になる。

### 4-7. その他

| デバイス | 状態 |
|---|---|
| USB0 | `dr_mode = "host"`、VBUS は `gpio3[9]` |
| RTC | dtsi で **`status = "disabled"`**。Brain では使われていない (要調査) |
| 電源断 | POWER ブロックの `poweroff` |
| 有線/無線 LAN | **無し**。dtsi の `reg_fec_3v3` 等は i.MX28 EVK からの残骸 |

ネットワークが無いため、`docs/tasks/network/` の LGY-98 / NE2000 系はすべて対象外になる。

---

## 5. 独自コードを起動する経路

brain-hackers が確立している経路は 3 つある。

| 経路 | 仕組み | OS32 から見た評価 |
|---|---|---|
| **BrainLILO** | 純正 WinCE 上でアプリ (`AppMain.exe`) として起動し、物理アドレスへ制御を移して U-Boot を実行する | 最も安全。**eMMC を書き換えないので失敗しても再起動で戻る**。最初はこれ一択 |
| **SD ブート** | i.MX28 のブート ROM がブートモードに従い SD から `.sb` ストリームを読む | ブートモード設定 (OCOTP / ブートモードピン) の変更が要る。要調査 |
| **eMMC ブート** | 純正領域を上書きして eMMC から起動する | 純正 WinCE を失う。**文鎮化リスクが最大**。復旧手順が確立するまで手を出す対象ではない |

**PW-SH4 以降は SH3 以前より起動の敷居が上がっている**という報告がある (バイナリ書き換えを要する、SH7 世代はさらに強固)。
PW-SH5 はその「敷居が上がった側」に属する。U-Boot 側には `pwsh5_defconfig` が揃っているので不可能ではないが、
手順の確認は実機と実物の資料でしか行えない。

U-Boot が入る前提に立てるなら、OS32 側の起動物は **U-Boot が `bootz` で読める形式 (zImage 相当 + ロードアドレス)** にするのが最短で、
`boot/` の IPL / ローダ (PC-98 の INT 1Bh、`.8086` 制約、LBA 2–17) は **全部不要になる**。

---

## 6. OS32 側の棚卸し — 何が残り、何が消えるか

現在のソース規模と、PW-SH5 での扱い。

| 領域 | 規模 (行) | PW-SH5 での扱い |
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

OS32 の開発サイクルは NP21/W ai-debug フォーク (HTTP デバッグサーバ + MCP: レジスタ・メモリ・逆アセンブル・ブレークポイント・トレース) に
完全に依存している。`make hotdeploy` も `/api/cmd` も `emu_read_mem` も PW-SH5 には無い。

- i.MX28 を実用的な精度で再現するエミュレータは知られていない (QEMU に mxs マシンは無い)。
- したがって **初期は「シリアル 1 本 + 実機 1 台」だけ**で進むことになる。カーネル中枢の作り直しをその条件でやることになる。
- これは技術的困難というより **開発速度が 1〜2 桁落ちる** という意味で最大の障害。

### 7-2. V86 サブシステムの消滅

`kernel/v86*` 一式 (C 6 + ヘッダ 6 + asm 3) と MS-DOS ゲスト実行は **x86 の仮想 8086 モードそのもの**であり、
ARM には対応する機構が無い。エミュレータを書く以外に道は無く、それは別プロジェクトである。
`docs/tasks/v86v2/` の成果は PW-SH5 では回収できない。

### 7-3. 非整列アクセスとキャッシュ

x86 では黙って通っていたコードが ARMv5 では壊れる。具体的に危ないのは:

- ext2 / FAT / ISO9660 / MGX / OS32X ヘッダなど、**バイト列に構造体ポインタを重ねている箇所**。
- `kstring` / `kmalloc` のアラインメント前提。
- GFX の 32bit 単位コピー最適化。
- DMA (LCDIF・MMC) と CPU キャッシュのコヒーレンシ。OS32 には現在キャッシュ管理の概念が無い。

移植の前段として **既存 C コードの非整列アクセス監査**が要る。これは実機が無くても今日できる作業である。

### 7-4. ABI と既存バイナリ資産

`sdk/kapi.json` を正典とする KAPI の**仕組み**は残せるが、中身の規約 ([C3] の `__cdecl`、System V i386 ABI、`int 0x80` スロット) は
すべて AAPCS + `svc` に置き換わる。`KAPI_VERSION` の連番は引き継げず、**別 ABI として切る**判断になる。
既にビルド済みの `.bin`、`libos32gui.shlib`、submodule のアプリ・ゲームは 1 つも動かない。

### 7-5. 制約規則の再定義

`docs/CONSTRAINTS.md` の [HW1] (EGC/GRCG/GDC 禁止・VRAM 直書き)、[HW2] (DMA 64KB 境界)、
[V1]〜[V3] (HostDrv / NHD / curl タイムアウト) は **PC-98 と NP21/W に固有**であり、
PW-SH5 では意味を失うか、別の規則 (キャッシュ管理必須、非整列アクセス禁止、シリアル検証) に置き換わる。
[C1]〜[C4] (C89 / kstring / 三層定数) はそのまま維持できる。

---

## 8. もし進めるなら — 段階案 (提案であり計画ではない)

各段は「シリアル出力で結果が確認できること」を完了条件にする。

| 段 | 内容 | 実機の要否 |
|---|---|---|
| M0 | 既存 C コードの**非整列アクセス / キャッシュ前提の監査**、ARM クロスコンパイラ (`arm-none-eabi-gcc`) の整備 | 不要 |
| M1 | 実機入手、分解調査、**DUART の引き出し**、BrainLILO + U-Boot の起動確認 (純正 eMMC は温存) | 要 |
| M2 | U-Boot から読める最小カーネルで **DUART に "hello"** | 要 |
| M3 | ICOLL + TIMROT で**割り込みとタイマ**、`sys_get_tick` 相当 | 要 |
| M4 | MMU + ページング、kmalloc、kselftest の ARM 版 | 要 |
| M5 | **LCDIF + ILI9805 で画面**。RGB565 バックエンド、KCG フォント描画 | 要 |
| M6 | GPIO マトリクスキーボード、LRADC タッチ | 要 |
| M7 | SSP/MMC ドライバ → 既存 VFS/ext2 の接続、シリアル rshell | 要 |
| M8 | KAPI ABI (AAPCS + `svc`) 再定義、`crt0` と実行フォーマット、userland 再ビルド | 要 |

M0 だけは今のリポジトリで単独に価値がある (x86 でも潜在バグの発見になる)。

---

## 9. 未確認事項 (実機または一次資料が要る)

1. **i.MX28 の正確な派生型番** (i.MX283 / i.MX287 など)。定格 454MHz に対し U-Boot が 480MHz を設定している理由も含めて未確認。
2. DRAM 128MB は DT と U-Boot の記述であり、**実チップの容量は未確認**。
3. 内蔵 eMMC の容量 (公表のユーザー領域 約 500MB とは別)。
4. **DUART パッドの物理位置**、分解の可否、信号電圧、必要な治具。
5. PW-SH5 で BrainLILO が実際に通るか、必要な手順 (SH4 以降の敷居が上がったという報告の具体的内容)。
6. ブートモードピン / OCOTP による SD ブートの可否と、設定を戻せるか。
7. RTC が dtsi で無効な理由 (存在しない / 未対応 / 電池が別)。
8. ブザー・音声出力の実装有無 (dtsi の枠は SH5 で有効化されていない)。
9. 電源管理 (充電、サスペンド、電池残量読み出し) の方法。
10. LCD の実測フレームレートと LCDIF 転送に要する時間。
11. 純正 WinCE の復旧手段 (文鎮化からの戻し方)。

---

## 10. 出典

一次情報 (コードを直接確認):

- brain-hackers / **u-boot-brain** — `configs/pwsh5_defconfig`, `board/sharp/pwsh5/{pwsh5.c,README}`, `board/sharp/common/{lcd.c,lcd.h,Kconfig}`, `include/configs/brain_mx28_common.h` : https://github.com/brain-hackers/u-boot-brain
- brain-hackers / **linux-brain** — `arch/arm/boot/dts/{imx28-pwsh5.dts,imx28-brain.dtsi,imx28.dtsi}`, `drivers/gpu/drm/tiny/brain.c`, `drivers/input/keyboard/brain-kbd-gpio.c` : https://github.com/brain-hackers/linux-brain
- brain-hackers / **brainlilo** : https://github.com/brain-hackers/brainlilo

二次情報:

- シャープ 製品ページ PW-SH5 : https://jp.sharp/support/dictionary/product/pw-sh5.html
- 価格.com スペック : https://kakaku.com/item/J0000026484/spec/
- Brainux (プロジェクト概要・Wiki) : https://brainux.org/ , https://wiki.brainux.org/
- 「詳解・電子辞書で Linux がブートするまで」 (Takumi Sueda) : https://speakerdeck.com/puhitaku/boot-linux-on-sharp-brain-explained
- NXP i.MX28 データシート (IMX28CEC) : https://www.nxp.com/docs/en/data-sheet/IMX28CEC.pdf
- OUCC「SHARP Brain 用アプリケーションの作成方法」 : https://oucc.org/blog/articles/303/

本調査で参照した外部リポジトリは一時作業領域にクローンしたのみで、本リポジトリには一切取り込んでいない。
