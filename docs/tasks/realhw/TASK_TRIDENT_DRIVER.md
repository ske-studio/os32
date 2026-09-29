# TASK_TRIDENT_DRIVER — 実機 Ra266 の内蔵 Trident (1023:9660) を GUI の画面にする (v3)

> 状態: **調査・設計票 v1 (2026-09-29)** — 実装なし。Codex 設計レビュー前 (PM が引き渡す)。
> 版: v1 (2026-09-29、初版)。発行: コーダー (Claude Code サブエージェント `claude-opus-5-5`)、依頼 PM。
> 対象: 実機 PC-9821Ra266 の内蔵アクセラレータ (PCI 0:8.0 `1023:9660`)。**NP21/W では動かせない** (§3-3)。
> 関係: [PLAN.md](PLAN.md) §1・§7、[TASK_PEGC480_REALHW.md](TASK_PEGC480_REALHW.md)、[TASK_LAN_82557.md](TASK_LAN_82557.md) §6 (PCI 列挙の実測)、
> [../settings/DEVICE_RESERVATION.md](../settings/DEVICE_RESERVATION.md)、[../v3/TASK_MEMMAP_V3.md](../v3/TASK_MEMMAP_V3.md)、
> [../v3/TASK_HAL_WIRING.md](../v3/TASK_HAL_WIRING.md) §1-4 (PCI の結線表)、[../gui/DESIGN.md](../gui/DESIGN.md) §2・§3・§6、[05_drivers.md §5-5](../../05_drivers.md)。

## 0. ユーザー決定 (2026-09-29) とこの票の範囲

> 「Cirrus はエミュレータ互換のためだけ。実機 (Ra266) は Trident。Trident のドライバが必要なら Trident のドライバを作成する」

- これで [PLAN.md](PLAN.md) §7 の「Trident は保留」と [TASK_PEGC480_REALHW.md](TASK_PEGC480_REALHW.md) §6 の「Trident のドライバは作らない」は
  **上書きされる** (両票には本票への参照を足した)。
- **この票は調査と設計まで**。コードは書かない。実装に入るのは §7 の決裁と Codex 設計レビューの後。
- 書き方の約束: `docs/hw/` (Bible / UNDOCUMENTED) と実測に**あること**と**無いこと**を分ける。無いことは「資料に記述なし」と書き、
  記憶や一般論で断定しない。一般論で補った所は「(一般値、docs/hw に記述なし)」と明記する。

## 1. 何を解決するのか — 今の症状と照らして

### 1-1. 今の実機の画面 (2026-09-25〜26 の記録)

| 経路 | 実機 Ra266 + 液晶での状態 | 出典 |
|---|---|---|
| `gfxmode pc98` (640x400x16、GDC 24kHz 系) | **正しく映る** | TASK_PEGC480 §0 |
| `gfxmode pegc` (640x480x256、GDC を 31kHz / 480 ラインへ切り替え) | **桁ずれ** (液晶が正しい位置で取り込めない) | TASK_PEGC480 §0 |
| CUI (起動時の PEGC 準備が同期を触っていた頃) | 桁ずれ → `pegc_prepare` で同期を触らないようにして消えた | TASK_FDC_REALHW §9-1、`include/gfx_hal.h` の `prepare` 注記 |
| `gfxmode cirrus` | Ra266 には CL-GD5430 (Xe10 型 ID 5Bh) が無いので probe が通らず 9801 へ落ちる (ユーザー決定で Cirrus は NP21/W 専用) | io_wab.md の搭載表に Ra266 の行は無い |

PEGC の桁ずれの原因は未確定 (TASK_PEGC480 §2 の H1〜H5)。NP21/W での `v86 -g` の記録では、BIOS は 480 ラインへ入るとき
**PITCH=80・GDC クロック 5MHz** にするのに `pegc_init` はどちらも設定していない (H5 の裏付け、TASK_PEGC480 §3)。
**実機の ROM の値はまだ採っていない** (`v86 -g` は実装済み・実機未実施)。

### 1-2. Trident で解決するもの・しないもの

| 観点 | PEGC (今) | Trident で見込めること | 確度 / 根拠 |
|---|---|---|---|
| **桁ずれの回避** | uPD7220 の SYNC・PITCH・GDC クロックを OS32 が直に書く。値は NP21/W 由来で、実機の ROM と一致するか未確認 | 映像の同期は Trident の CRTC と画素クロック生成器が作る。**GDC を 480 ラインへ切り替えない** (98 側は 400 ラインの CUI のまま置いておける) | **中**。同期の値は結局 OS32 が書く (VGA 互換 CRTC + 拡張レジスタ + クロック生成器) ので「値が正しければ直る」のは PEGC と同じ。違いは、VGA 系の標準タイミングは液晶側が前提にしている形式だという点 (一般論、docs/hw に記述なし)。ただし**レジスタ資料が手元に無い** (§3) |
| **液晶の水平/垂直周波数** | 31kHz / 480 ライン。液晶の OSD で実値を見る段 0 (TASK_PEGC480) は未実施 | 640x480@60 なら VGA 標準の約 31.5kHz / 約 60Hz、800x600@60 なら約 37.9kHz / 60Hz、1024x768@60 なら約 48.4kHz / 60Hz (一般値、docs/hw に記述なし。VESA DMT は未入手) | 液晶の対応範囲はユーザーの機種次第 (**型番・対応周波数の記録なし**) |
| **解像度** | 640x480 固定 (PEGC の VRAM 512KB で 640x480x8 = 300KB) | 800x600 / 1024x768 | VRAM 容量は PLAN §1 の「2MB」(出典は pc-9800.net の機種 DB、実機で未確認)。2MB なら 1024x768x8 = 768KB の面が 2 枚 (表示 + クライアント) 入る (計算) |
| **色数** | 256 色 (PACKED8) | 8bpp のまま (推奨)。16bpp は可能性があるが GUI・libos32gfx 全体に波及 (§4-4) | チップの対応色深度は資料に記述なし |
| **描画速度** | CPU が主記憶の BB (300KB) → F00000h の窓へ転送 | (a) 表示面とクライアント面をカード VRAM に置き、**present をエンジン BLT** にする (Cirrus と同じ形)、(b) エンジンの塗り / 転送 (HW_FILL / HW_BLT) | エンジンのレジスタは資料に記述なし。PEGC 窓と Trident の LFB のどちらが速いかは**実測していない** (Ra266 の PEGC も PCI の 1033:0009 の先にある — io_pci.md 表 2 の「98ｸﾞﾗﾌｨｯｸｽ」、実測 lspci) |
| **テキスト面の合成** | 合成あり (TEXT_OVERLAY) | **無くなる見込み** — 映像出力を 98 側から切り替えるので、テキスト VRAM は映らない (Cirrus と同じ、DESIGN §1 R4) | 切り替えの手段が資料に無い (§2-4) |
| **PEGC の 15MB 穴 (F00000h〜)** | 043Bh bit2=1 (16MB 以上を RAM) の構成では PEGC 窓が使えない | Trident の BAR は 0x2000_0000 帯 (実測) で RAM の穴を要らない | 実測 (lspci) |

**要するに**: Trident は「GDC の 480 ライン切り替えを使わずに済む」ことで桁ずれを**回避しうる**経路であり、解像度と
描画の上積み (エンジン BLT) を伴う。ただし**レジスタ資料が無い**ので、確度は資料の入手 (§3) で決まる。
PEGC の直し (TASK_PEGC480 段 1〜2) の方がはるかに安く、両方を残す意味がある (§7 決裁 T1)。

## 2. Trident が Ra266 にどう見えるか

### 2-1. 実測 (2026-09-23、`lspci`、TASK_LAN_82557 §6)

```
0:7.0 1033:0009 NEC display class 03.80        ← 98 グラフィック (PEGC 側)
0:8.0 1023:9660 Trident  irq 255 (未割当)
      bar0 mem 0x20000000 / bar1 mem 0x20400000 / bar2 mem 0x20800000
0:11.0 8086:1229 Intel 82557 bar0 mem 0x20410000 ...
```

- **BAR は 3 本とも memory**。I/O BAR は無い。大きさは測っていない (`drivers/pci.h` は BAR に書かない方針 — 大きさを測るには全 1 を書いて読み戻す必要がある)。
  **隣の割り当てからの推定** (未確認): bar1 は 82557 の bar0 (0x20410000) の手前なので **64KB 以下**、bar0 は 0x20400000 の手前なので **4MB 以下**。bar2 の上限は推定できない。
- Command / Status / Revision / Class / Expansion ROM BAR (0x30) は**まだ読んでいない** — `lspci -v 0:8.0` / `pcidump 0 8 0` で取れる (段 0)。
- IRQ は未割当 (255)。割り込みを使わない作り (VSYNC はポーリング) なら困らない。

### 2-2. 資料にあること (UNDOCUMENTED `io_pci.md` / `io_wab.md`)

| 事実 | 出典 | Ra266 に当てはまるか |
|---|---|---|
| PC-9821Xa16・Xa13・Xa12・Xa10・Xa9・Xa7・Xv13 の PCI 表: デバイス 01000b (= 8) が「内蔵ｱｸｾﾗﾚｰﾀ 1023h(Trident) 9660h(TGUI9680XGi)」、00111b (= 7) が「98ｸﾞﾗﾌｨｯｸｽ 1033h 0009h」 | io_pci.md 表 2 | **番号は実測と一致** (dev 7 = 1033:0009、dev 8 = 1023:9660)。ただし Ra266 は表の対象機種に**無い**。C バスブリッヂは表では dev 6 = 1033:0001、実測は dev 6 = **1033:002C** (別品) |
| 内蔵アクセラレータの機種表: Xa16〜Xa7・Xv13 は TGUI9680XGi | io_wab.md | Ra266 の記述なし。PLAN §1 は機種 DB から **TGUI9682XGi** とする。**9680 と 9682 は同じ device ID 9660h** なので lspci では区別できない (区別の仕方は資料に記述なし) |
| **0FACh** (PCI バス搭載機): bit1 = 1 で**内蔵アクセラレータ** / 0 で **98 グラフィックス** (Xa10・Xa9・Xa7・Xa12・Xa7e)、bit0 = RGB IN スルー、bit7〜2 は常に 111111b | io_wab.md 0FACh | 対象機種に Ra266 は**無い**。同じ作りかは実測で確かめる (段 1 で読むだけ — 読むコマンドが今は無い) |
| 0FAAh / 0FABh (内蔵アクセラレータ制御、レジスタ 00h = ID) | io_wab.md | 対象は Cirrus / S3 の内蔵機。**Trident 機の記述は無い** |
| 内蔵 CL-GD5428/5430 は VGA レジスタを 0CA0h〜0CAFh 等へ**写し替えて**いる | io_wab.md | Trident のポートの写し替えは**資料に記述なし** |
| PCI-C バスブリッヂのメモリ領域レジスタ (40h〜4Ch / 50h〜5Ch)、PCMC の FBR (7Ch) 「128K VGA 領域属性」 | pci_cbus.md、pci_pcmc.md | 対象は Wildcat / Xa 系。Ra266 (82441FX) のブリッヂは別品で**記述なし** |

### 2-3. VGA 互換ポートの置き場所 — **資料に記述なし、しかも衝突の恐れがある**

Trident の VGA 互換レジスタ (3C0h〜3DFh) が PC-98 の I/O 空間のどこに出ているか、**資料に記述なし**。
PC/AT の番号 (3C0h 系) のまま出ているなら、Bible §4-3 の I/O マップの**部分デコード**に当たる:

| VGA のポート | 下位バイト | Bible 4-3 で同じ下位バイトを拾う装置 (上位バイトは X = 見ない) |
|---|---|---|
| 3C8h・3CAh・3CCh・3CEh (DAC 書き index・Feature 読み・Misc 読み・GC index) | C8h〜CEh の偶数 | **640KB FDD コントローラ 765 相当** (表の 26) |
| 3C1h〜3CFh の奇数 (Attr 読み・Seq data・DAC data・GC data ほか) | C1h〜CFh の奇数 | **GP-IB 7210 相当** (表の 27) |
| (Trident のクロック設定 43C8h/43C9h — PCem 由来のソースの番号) | C8h・C9h | 同上 |

PCI ホストブリッヂがこれらを PCI 側で先に受けるのか、C バスへ流すのかは**資料に記述なし**。
NP21/W の PCI 型 Cirrus は「PCI 版はポート番号そのまま」(3C0h 系、`cirrus_vga.c` の `vga_convert_ioport`) だが、
これは**エミュレータの作りであって実機の仕様ではない**。→ **段 0 では 3Cxh 帯に I/O を出さない** (FDC を叩く恐れ)。
残る候補は (A) 3C0h 系のまま (Command の I/O Space Enable が立っているか)、(B) NEC 流の写し替え (Cirrus の 0CA0h 系と同様)、
(C) BAR の MMIO (bar1 の 64KB 以下の窓が候補)。どれかは**段 0〜1 の実測**で決める。

### 2-4. 98 グラフィックとの共存 (映像の切り替え)

- Ra266 のモニタ端子は 1 つで、98 グラフィック (PEGC を含む) と Trident の出力をどう切り替えるかは**資料に記述なし**
  (Xa 系なら 0FACh bit1、§2-2)。
- 9821 の BIOS / GDC は 98 グラフィック側の話で、Trident には触らない (資料の範囲)。**PC-98 の起動時に Trident が初期化されるか**
  (VGA BIOS の実行、メモリクロック・画素クロックの設定) は**資料に記述なし** — 段 0 でレジスタの初期値を読めれば分かる。
- CUI (テキスト VRAM) は Trident の出力中は**映らない**見込み (§1-2)。**パニック・例外・CTRL+STOP で必ず 98 側へ戻す**経路が要る
  (Cirrus の `leave` / リレーと同じ責務)。戻せなかったときの復旧はシリアルの rshell とリセット。

### 2-5. リニアフレームバッファ

- bar0 (0x20000000、4MB 以下の推定) が VRAM のリニア窓である見込みが高いが、**資料に記述なし** (段 1 で「書いて映るか」で確かめる)。
- NP21/W のソース (`src/wab/tgui9680.c`、PCem の `vid_tgui9440.c` 由来) は PCI の device ID を **9440h**・BAR を **1 本** (リニア窓) で返す作りで、
  **実機の 9660h・BAR 3 本と合わない**。参照実装としても Ra266 の構成の根拠にはならない。

## 3. 資料

### 3-1. `docs/hw/` にあるもの

- `undocumented/io_pci.md` (機種別の PCI デバイス番号、C バスブリッヂの割り込みルータ)、`io_wab.md` (内蔵アクセラレータの機種表・0FACh)、
  `pci_cbus.md` / `pci_pcmc.md` (Xa 系のブリッヂ)、Bible §4-3 (I/O マップの部分デコード)。
- **Trident のチップのレジスタ資料は無い** (`docs/hw/` を `trident|tgui|9660|cyber` で検索、2026-09-29)。

### 3-2. 必要な情報と入手方法 (入手可能性は未確認 — ユーザー判断 T3)

| 必要な情報 | 用途 | 入手先の候補 | 注意 |
|---|---|---|---|
| TGUI9680 / 9682 のデータブック (拡張シーケンサ・拡張 CRTC・リニア窓の制御・DPMS・**画素クロック生成器の式**・メモリクロック) | モード設定そのもの | Trident のデータブックの PDF (古い資料のアーカイブ類) | 所在・版は未確認。**入手したら `docs/hw/` に置く (コミット禁止)** |
| 2D エンジン (GER) のレジスタ | present の BLT、HW_FILL / HW_BLT | 同上 | 同上 |
| 参照実装 | 資料の読み違いの突き合わせ | X.Org `xf86-video-trident` (MIT/X11)、Linux `tridentfb.c` (GPL)、86Box / PCem `vid_tgui9440.c` (GPL) | **OS32 は MIT**。GPL のコードは読むだけで写さない。MIT/X11 のものでも出典を明記 |
| Ra266 固有の結線 (VGA ポートの置き場所、映像の切り替え、起動時に Trident が初期化されるか) | §2-3・§2-4 | **実測** (段 0〜1)。補助に NEC の Win9x 用ディスプレイドライバ (Ra/Xa 内蔵 Trident 用) の INF・設定 | ドライバ本体の逆アセンブルは使用許諾の確認が要る (ユーザー判断 T3) |
| 液晶モニタの対応周波数 | 目標のモード | モニタの型番と取扱説明書 (ユーザー) | 型番は票に記録が無い |

### 3-3. NP21/W は Trident を再現しない — 検証は実機だけ

- DESIGN §4 のとおり `SUPPORT_TRIDENT_TGUI` は x64 Release に**入っていない**。`tgui9680.h` / `tgui9680_extern.h` は 0 バイトで結線されていない (PLAN §7)。
- NP21/W は PCI の設定空間 (0CF8h) も再現しない (PLAN §5)。**Trident の経路は probe の手前で必ず落ちる** — NP21/W での回帰は
  「落ちて PEGC / 9801 へ譲る」ことだけ。
- NP21/W 側の結線 (PLAN §7 の (a)) は**この票の作業ではない**。結線しても PCem の 9440 相当 (§2-5) で、実機の構成とは違う。

### 3-4. 実機で採る診断 (`v86 -g` と同じ考え方の案)

| 名前 (案) | 何を採るか | 副作用 | 段 |
|---|---|---|---|
| 既存 `lspci -v 0:8.0` / `pcidump 0 8 0` / `pcidump 0 7 0` / `pcidump 0 0 0` | Trident と 98 グラフィックの config 256 バイト (Command の I/O・Mem・Palette Snoop、Revision、Class、Expansion ROM BAR、Subsystem)、ホストブリッヂ (82441FX) の設定 | なし (読むだけ、実装済み) | 0 |
| `trident -r` (新規、CPL0 の管理者用診断) | 0FACh の読み値、(A)〜(C) のうち**安全と決めた経路**でのレジスタの初期値の全ダンプ | 経路次第。3Cxh への I/O は §2-3 の衝突が否定されるまで出さない | 1 |
| `trident -t` (新規) | リニア窓へ縦線と行番号のテスト画 (TASK_PEGC480 段 0 の絵と同じ) を描き、映像を切り替え、N 秒後に**必ず 98 側へ戻す** | 映像の切り替え (戻しは時間で強制) | 2 |
| (任意) Windows 上の状態の採取 | Ra266 に Win9x が入れば、NEC のドライバが 640x480 / 800x600 を出している状態のレジスタを DOS 窓などから採る | ユーザーの環境次第 (T3) | 補助 |

**BAR の大きさを測る書き込み** (全 1 を書いて読み戻し、元へ戻す) は `drivers/pci.h` の方針 (BAR に書かない) を破る。
要るなら決裁 T4。隣の割り当てからの推定 (§2-1) で足りるならしない。

## 4. 既存の構造にどう載せるか

### 4-1. 層 (DESIGN §6 の二層構造をそのまま使う)

```
gfx/backend_trident.c      GfxBackend (probe / prepare / init / query / present_rect / set_palette /
                           enter / leave / fill_rect / blit / shutdown)。Cirrus と同じ「アクセラレータ系」
  ├─ drivers/wab_tgui.c    チップドライバ: VGA の論理番号 (3C0h 系、wab_glue.h) と拡張レジスタだけを知る
  └─ drivers/wab_glue_ra266.c (仮)  ボードグルー: PCI の BAR から窓の番地、ポートの翻訳 (§2-3 の結論)、
                           映像の切り替え (§2-4 の結論)。struct pci_driver の probe から見つける
```

- **WabGlue の契約 (`drivers/wab_glue.h`) で足りる見込み**: `out/in` (翻訳)、`relay` (映像の切り替え)、`lin_base/lin_size` (bar0)、
  `mmio` (エンジンのレジスタが MMIO なら)。違うのは**番地が定数ではなく BAR から実行時に決まる**ことだけ。
  §2-3 で (C) MMIO 経路になった場合は `out/in` を MMIO で実装する (契約は変えない)。
- **PCI の結線表 (`drivers/pci_bind.c`) に載せるか**: 載せる場合、probe は `pci_bind_all` の時点 (起動時) に走り、
  gfx の `prepare` とは時期が違う。案: pci_bind の probe は**装置に触らず** (DECLINE も QUARANTINE も出さない形で) BDF と BAR を
  控えるだけ、実際の初期化は gfx の `init` で行う。pci_bind の probe 契約 (触る前に STARTING、(1)〜(7) の順) は NIC 向けに
  書かれていて、表示装置には合わない所がある → **Codex 論点 D2**。
- 選択: `GFX_PREF_TRIDENT` (= 4) を `include/gfx_hal.h` に足し、`gfxmode trident` / `system.cfg GFX=trident` で強制。
  **auto の probe 順に入れるか**は決裁 T5 (推奨: 段 4 で実機の往復が通るまで auto には入れない)。
- 能力ビット: `TEXT_OVERLAY` 無し、`HW_FILL` / `HW_BLT` は段 5 でエンジンが動いてから。

### 4-2. メモリ・窓 (v3 のメモリマップと DEVICE_RESERVATION)

- BAR は 0x2000_0000 帯 (実測)。**RAM とも PEGC の 15MB 穴とも重ならない**ので、DEVICE_RESERVATION の「RAM を窓に明け渡す」
  問題は起きない見込み。ただし broker の台帳 (DEVICE_RESERVATION §4) には **MMIO の owner として登録**する (UNKNOWN / 管理上限外の窓も記録できる設計)。
- 写像: Cirrus は `paging_map_phys(lin_base, lin_base, …)` で**恒等写像**している。0x20000000 を恒等に張ると master の PDE 128 番を
  使う (動的 PT。`kernel/paging.h` の注記どおり master CR3・live AS なしの時点でしか新しい PT を作れない)。
  **別担当で進行中のデバイス窓の帯 [0xFE000000, 0xFF000000) 案**が入るなら、窓は「物理 BAR → 帯の中の仮想番地」に張るのが筋で、
  バックエンドは**恒等写像を前提にしない** (`bb_base` は仮想番地、`gfx_bb_phys_range` は物理番地を返す) 作りにしておく。
  帯の大きさ 16MB に対し、Trident の必要量は bar0 (≤4MB、推定) + bar1 (≤64KB) (+ bar2 を使うなら不明)。→ **Codex 論点 D4**、帯の担当と調停。
- CPL=3 へは**クライアント面だけ**を USER で見せる (契約 G4、`paging_addrspace_map_user_keep`、PCD を保つ)。表示面は supervisor。
  `ring3_guard trident` の否定試験を足す (Cirrus / PEGC と同じ)。
- バックバッファはカード VRAM の非表示領域 (Cirrus と同じ)。主記憶の予約 (`sys_reserve_top`) は要らない。

### 4-3. KAPI / ABI への影響

| 案 | 影響 |
|---|---|
| **8bpp (PACKED8) のまま、640x480 から** (推奨) | **KAPI 変更なし**。`GFX_ScreenInfo` の width / height は u16、format は `GFX_FMT_PACKED8`、能力ビットも既存で足りる。`GFX_PREF_TRIDENT` はカーネル内部のヘッダ (`gfx_hal.h`) で KAPI ではない |
| 800x600 / 1024x768 (8bpp) | KAPI 変更なし。ただし GUI / アプリが契約 G5 (決め打ちしない) を守っているかの確認が要る (gshell・libos32gui のレイアウト、壁紙・アイコンの配置)。exec が USER で張るクライアント面が 1024x768x8 = 768KB に増える |
| 16bpp | `GFX_FMT_*` の追記 (末尾のみ、[ABI2]) と版上げ・`make clean` ([ABI3])、**libos32gfx・libos32gui の描画・文字・パレット貸し (G8) の全経路**に 16bpp を足す。GUI 1.x の範囲を超える → 決裁 T6 (推奨: しない) |

### 4-4. カーネルの大きさ

カーネル本体の残りは **21KB** (`docs/02_memory.md` §2-1、2026-09-29 時点の文面)。Cirrus 系の実績は `.text` だけで
`wab_glue_xe10.o` 0x19C + `wab_cirrus.o` 0xDAD + `backend_cirrus.o` 0xB4B ≈ **6.6KB** (手元の `build/out/kernel.map`)。
Trident は同程度〜それ以上 (モード表・クロック計算・PCI 結線) を見込み、**残りの 1/3〜1/2 を食う**。
v3 の「ドライバの動的読み込み」([../v3/PLAN.md](../v3/PLAN.md) §3) の前に静的に積むかは決裁 T7。

### 4-5. [HW1] との関係

[HW1] は 98 側 (EGC / GRCG / GDC) の描画命令の禁止。アクセラレータの 2D エンジンを使ってよいことは DESIGN §2 B3 にあり、
Cirrus は既に使っているが、**`docs/CONSTRAINTS.md` の [HW1] の本文には追記されていない**。Trident でエンジンを使うなら、
この追記を同時に済ませるのが筋 (本票では直さない — 規則の変更は PM・ユーザーの判断)。

## 5. 段階案

| 段 | やること | 前提 | 検証 | 実装の量 |
|---|---|---|---|---|
| **0** | **実機で読むだけ**: `lspci -v 0:8.0`・`pcidump 0 8 0`・`pcidump 0 7 0`・`pcidump 0 0 0`、(0FACh の読み値は、I/O ポートを読む既存コマンドが無いので段 1 の `trident -r` で採る)、液晶の型番と OSD の周波数 (TASK_PEGC480 段 0 と同じ回で) | なし | 実機 (シリアル rshell) | **0** (既存コマンドのみ) |
| **資料** | §3-2 の入手 (データブック・参照実装)。入手できた範囲でモード設定 (640x480x8) の手順を票へ起こす | ユーザー判断 T3 | 机上 | 0 |
| **1** | `trident -r` (CPL0 診断): 段 0 で決めた安全な経路でレジスタの初期値を全部採る。**書き込みはしない**。3Cxh 帯は §2-3 の衝突が否定されるまで使わない | 段 0、資料 | 実機 | 小 (純粋部はホスト試験) |
| **2** | `trident -t`: 640x480x8 のモード設定 → リニア窓へテスト画 → 映像を切り替え → **N 秒で必ず 98 側へ戻す** (失敗経路も同じ戻し)。液晶の OSD を写真で | 段 1 | 実機 (写真) | 中 |
| **3** | `gfx/backend_trident.c` (8bpp、CPU で present)。`GFX=trident` 強制時だけ。gshell の往復、CTRL+STOP、パニック時の戻し、`ring3_guard trident` | 段 2 | 実機 + NP21/W 回帰 (probe が落ちて PEGC/9801 へ譲るだけ) | 中〜大 |
| **4** | 解像度 (800x600 / 1024x768) と GUI の G5 の確認 | 段 3、決裁 T6 | 実機 | 中 |
| **5** | エンジンで present (表示面 ← クライアント面) と HW_FILL / HW_BLT | 段 3、エンジンの資料 | 実機、`gfx_stats` | 中 |

ホスト試験に切り出せるもの: 画素クロックの M/N/K の計算 (資料の式が手に入れば)、モード表の整合 (総幅 > 表示幅・帰線の位置)、
BAR の生値の読み方 (`pci_decode.c` の延長)、選択の順序 (`GFX_PREF_TRIDENT` の落ち方)。

## 6. 実機でしか分からないこと / 手元で済むこと

- **実機でしか分からない**: §2-3 のポートの置き場所、§2-4 の映像の切り替え、BAR の中身 (どれが VRAM / MMIO か)、起動時の初期化の有無、
  VRAM 容量、液晶が受けられるモード、LFB と PEGC 窓の書き込み速度の差。
- **手元で済む**: 選択とフォールバック (NP21/W で `GFX=trident` が PEGC / 9801 へ落ちる)、純粋部のホスト試験、KAPI 変更が無いことの確認、
  帯の写像の単体試験 (帯の担当の試験に乗る)。

## 7. ユーザーの判断が要る点

| # | 論点 | 選択肢 | 推奨 |
|---|---|---|---|
| T1 | PEGC の桁ずれ (TASK_PEGC480) を続けるか | (a) 続ける (次の実機の回で `v86 -g`) / (b) Trident に一本化して止める | **(a)**。`v86 -g` は実装済みで実機 1 回で済む。PEGC は NP21/W で検証できる唯一の 256 色経路で、Trident が動かない機種・状況の退避先 |
| T2 | 最初の目標 | (a) 640x480x8 で桁ずれを消すまで / (b) 最初から 800x600 以上 | **(a)**。KAPI・GUI に触れずに済む |
| T3 | 資料の入手 | データブックを探す / 参照実装 (MIT の X.Org、GPL は読むだけ) / NEC の Win9x ドライバの解析 / Windows を実機で動かして状態を採る | 入手先はユーザーの判断。**資料が無いまま段 2 以降へは進まない** |
| T4 | BAR の大きさを測る書き込みを許すか | 許す (割り込み禁止で書いて戻す) / 許さない (推定のまま) | 段 0 の結果を見て決める |
| T5 | auto の probe 順に Trident を入れる時期 | 段 3 から / 段 4 の往復が通ってから | **段 4 の後** |
| T6 | 16bpp をするか | する / しない | **しない** (GUI 全体に波及) |
| T7 | 動的読み込み (v3 §3) の前に静的に積むか | 積む (残り 21KB の 1/3〜1/2) / 動的読み込みを待つ | 段 2 までは診断コマンド (CPL0 アプリ) で進められるので、**段 3 の着手時に決める** |

## 8. 設計レビュー (Codex) に渡す論点

- **D1 レジスタの経路**: §2-3 の (A) 3C0h 系 / (B) NEC 流の写し替え / (C) MMIO のどれかを段 0〜1 でどう安全に見分けるか。
  3Cxh が 640KB FDD と GP-IB の部分デコードに当たる件 (Bible 4-3) の扱い、MMIO を読むだけでも副作用がありうる (FIFO など) 件。
- **D2 pci_bind との結線**: NIC 向けの probe 契約 (STARTING → I/O Enable → reset → … → RUNNING) を表示装置に当てはめるか、
  pci_bind は BDF と BAR を控えるだけにして初期化は gfx の `init` に任せるか。QUARANTINE の意味 (映像を切り替えたまま落ちた) の定義。
- **D3 映像の戻し**: パニック・例外・CTRL+STOP・アプリの異常終了で**必ず 98 側へ戻す**経路 (Cirrus の `leave` / リレーの教訓: `s_relay_on` の食い違いで黒画面)。
  戻しの手段 (0FACh 相当) が実機で未確認のまま段 2 に入らない条件の置き方。
- **D4 窓の写像と帯**: 0x2000_0000 帯の BAR を恒等に張るか、別担当のデバイス窓の帯 [0xFE000000, 0xFF000000) の仮想番地へ張るか。
  帯の容量配分 (Trident ≤4MB+64KB 推定、Cirrus 2MB、PEGC 512KB、82557 の BAR)、DEVICE_RESERVATION の台帳への MMIO の登録、
  動的 PT を作れる時期 (master CR3・live AS なし) と GUI 起動の境界。
- **D5 資料に無い前提**: VRAM 2MB (機種 DB 由来)、チップが 9680 か 9682 か (同じ 9660h)、起動時に Trident が初期化されているか、
  bar0 がリニア窓であること、画素クロックの式 — どれも**未確認のまま設計に入れていないか**。
- **D6 実機でしか確かめられない点の受け入れ方**: NP21/W の回帰は「落ちて譲る」だけで、機能の受け入れは全部実機の写真と rshell の出力になる。
  [V4] に沿って「未確認」をどう票に残すか。
- **D7 カーネル予算**: 残り 21KB に対して Cirrus 系 6.6KB の実績。静的に積むか、段 2 までを CPL0 の診断アプリに留めるか。

## 9. 受け入れ (段 3 の時点)

- P1: 実機で `GFX=trident` の GUI が正しい位置に出る (写真。液晶の OSD の周波数も)。GUI を抜けて CUI に戻っても正しい (テキストが映る)。
- P2: 実機で CTRL+STOP・アプリの異常終了・`crash` のあとも CUI が映る (戻しの経路)。
- P3: `ring3_guard trident` で表示面への書き込みが kill される (契約 G4)。
- P4: NP21/W で `GFX=trident` が probe で落ちて PEGC / 9801 で今どおり映る (`/api/screenshot`)。KAPI の版が変わらない (8bpp の場合)。
- P5: 段 0〜2 の記録 (config ダンプ、レジスタの初期値、OSD の値) を票に残す。資料に無い値は**出典を「実機 Ra266 で採取」**と書く。

## 10. しないこと

- NP21/W 側の `tgui9680.c` の結線 (OS32 の作業ではない、§3-3)。
- Cyber9320 など他の Trident 機・他機種の内蔵アクセラレータ (Ra266 の 1 台に絞る)。
- 資料が無いまま推測でレジスタを書くこと (段 1 は読むだけ、段 2 は資料の入手後)。
- Cirrus のバックエンドを消すこと (NP21/W の回帰経路として残す — ユーザー決定)。
- 16bpp (T6 の決裁まで)。
