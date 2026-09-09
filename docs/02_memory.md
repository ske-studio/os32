## 第2部 メモリマップ

### 開発方針と現在の実装上限

- **最低動作環境の数値は未確定**。「CUI 最低 8MB」という記述が 2026-09-09 に
  一時入っていたが、根拠が無く、既存の設計制約と矛盾するため撤回した。
  現時点で言えるのは設計上の下限が **物理 9.6MB 構成以上**であること
  ([tasks/gui/DESIGN.md §9.3](tasks/gui/DESIGN.md)、2026-09-04)。
  理由は CPL=3 アプリのスタックが物理 0x7C0000〜0x7FFFFF に固定で、
  8MB ちょうどではアプリ帯がホットデプロイ窓と重なるため。
- **CPL=3 のアプリ帯は 4MB (PDE) 単位で伸びる** (2026-09-10、票
  [tasks/memory/APP_BAND_PDE.md](tasks/memory/APP_BAND_PDE.md))。
  2026-09-09 までは PDE 1 枚固定の 3MB 窓 (0x500000〜0x800000) で、
  `RING3_USTACK_TOP` も `RING3_HEAP_TOP` も定数だったため **RAM を増やしても
  1 アプリの利用可能量は増えなかった** (8MB でも 15MB でも `heap_test` の
  レイアウトは完全に同一、avail 2,605,056 バイト)。
  現在はアプリ固有 PDE を **1〜2 枚** (`MEM_APP_BAND_MAX_PDES`) 取れる:

  | 枚数 | 帯 | 条件 |
  |---|---|---|
  | 1 (既定) | 0x400000〜0x7FFFFF | OS32X ヘッダの `heap_size` が 0、または 1 枚に収まる。**従来と完全に同じレイアウト** |
  | 2 | 0x400000〜0xBFFFFF | `heap_size` が 1 枚に収まらず、かつ空き RAM が届く (子の claim 範囲 A の末尾まで) |

  上限が 2 枚なのは PDE 3 (0xC00000〜0xFFFFFF) に PEGC のリニア窓 0xF00000 が
  入るため (`MEM_APP_BAND_DEVICE_FLOOR`)。枚数は `paging_app_band_pdes()` が
  決め、`exec/exec.c` の `RING3_USTACK_TOP` / `RING3_HEAP_TOP` は
  そこから導かれる実行時の値になった。空き RAM が足りず要求が入らないときは
  切り詰めずに `EXEC_ERR_NOMEM` で拒否する (スワップは持たない)。
- **ユーザーメモリを連続させる** (2026-09-09 方針)。目的は「1 アプリに渡せる
  連続領域を最大化すること」であって、物理末尾を空けておくことではない。

  ```
  システム - ユーザー - システム   OK  (末尾 1MB が予約でも構わない)
  ユーザー - システム - ユーザー   NG  (ユーザー帯に穴が開く)
  ```

  末尾側の予約 (`sys_reserve_top`) はこの形を保つ限り問題ない。禁じるのは
  **ユーザー帯の内側を割ること**。したがって予約は
  「使用可能上限の直下から連続して」取り、アプリ帯 (CPL=3 は 0x500000 から
  枚数ぶん、最大 0xC00000) に食い込ませない。食い込む構成は**その RAM 量を非対応とする**方が、
  帯に穴を開けるより良い。
  この方針でホットデプロイの物理末尾 256KB 窓を撤去した — 窓は CPL=3 スタック帯と
  完全に同じ範囲で、8MB 構成ではユーザー帯を割っていた。ユーザーランドの配送は
  HostDrv (`make deploy` → ゲストの `hsync`) に一本化した。
  装置が実際に占める帯 (PEGC の 0xF00000、Cirrus の 0x1000000) は装置側の事実なので
  `physmem` のモデルで MMIO として扱う。
- **PEGC (640x480) を使う GUI の下限は物理 9MB** (2026-09-09 実測)。バックバッファ
  300KB を `sys_reserve_top` が末尾から取るので、それがアプリ帯 (既定 1 枚なら
  0x500000〜0x800000) の外に収まる必要がある。8MB では収まらず食い込む (実測: 予約が 0x7B5000 から
  始まりアプリ帯と重なる)。9MB では 0x8B3000 から始まりアプリ帯の外
  (`hal_test` = `backend pegc 640x480 bpp=8`、`heap_test` の overlap check OK)。
  PEGC VRAM は 512KB しかなく (Bible 3-2)、640x480x8 = 307200 の 2 面は入らないので
  バックバッファは主記憶に置く。640x400 なら 2 面が VRAM に収まり
  `I/O 00A4h` のハードウェアページフリップが使える (UNDOCUMENTED io_disp 452-460、846)
  ため主記憶は不要になるが、480 ラインは採らない選択になる。
- GUI の必要 RAM はバックエンド・常駐領域・アプリの実測から別途定義する。
  GUI 開発を特定の容量に収めることや、8MB GUI 互換維持を暗黙の受入条件にしない。
- 32bit フラットアドレス空間の設計対象は 4GiB（0x00000000〜0xFFFFFFFF）。
  これは全域が利用可能 RAM、単一アプリの利用可能領域、または実装済み容量を意味しない。
  ROM・MMIO・予約帯・ガード・カーネル領域を区別し、物理 RAM の利用可能範囲を管理する。
- 下記の 16MB RAM 管理・32MB マッピングは現行実装の制約であり、製品の設計上限ではない。
  大容量対応には RAM 検出、ページ割当、ページ表、exec 配置、デバイス窓、媒体・転送用予約、
  境界演算を一貫して拡張し、検証する。定数だけの拡大で対応済みとは扱わない。
- 4GiB の exclusive end は u32 に格納できない。上端・長さの計算は広い整数または
  明示的なページ数表現を用い、wrap による範囲検査の通過を禁止する。
- 現開発段階では ABI 変更を許容し、kernel・SDK・userland・apps・game の
  クリーン再ビルドと配備整合性で揃える。既存データの保全とは別の方針である。

### §2-1 物理メモリ配置

> 番地の定義の正典は `include/memmap.h`。ここが食い違ったらそちらが正しい。
> カーネルスタックは 0x90000 から 0x1FC000 へ移動済み (V86 ゲストに
> 640KB を渡すため)。0x9FFFC は今もローダー段の ESP として使われるが、
> カーネルは `kentry.asm` で `MEM_KSTACK_TOP` に張り替える。

```
アドレス範囲              サイズ    用途                                    属性
─────────────────────────────────────────────────────────────────────────────
[ コンベンショナルメモリ (0x00000 - 0xFFFFF) ]
0x00000 - 0x00FFF         4KB      NP (NULLポインタ検出, ブート後)           NP
0x01000 - 0x49FFF         ~292KB   フォントキャッシュ (kcg.c, ブート後配置)   R/W
0x4A000 - 0x69FFF         128KB    Unicode-JIS変換テーブル (utf8.c)          R/W
0x6A000 - 0x89FFF         128KB    GFXバックバッファ (32KB × 4プレーン)      R/W
0x8A000 - 0x8BFFF         8KB      空き (将来用)                            R/W
0x8C000 - 0x8CFFF         4KB      ホットデプロイ制御ブロック                R/W
                                   (MEM_HOTDEPLOY_DESC)
0x8D000 - 0x8FFFF         12KB     空き                                     R/W
0x90000 - 0x90FFF         4KB      自動プレイ観測用メールボックス            R/W
0x91000 - 0x9FFFF         60KB     空き / V86 ゲスト窓の一部                 R/W
0xA0000 - 0xEFFFF         320KB    VRAM (テキスト + グラフィック)             R/W
0xF0000 - 0xFFFFF         64KB     BIOS ROM                                  R/O

[ カーネル帯域 (0x100000 - 0x1FFFFF, 1MB) — 動的レイアウト ]
0x100000 - __bss_end      ~200KB   カーネルバイナリ (.text + .data + .bss)    R/W
KHEAP_BASE - +320KB       320KB    カーネルヒープ (kmalloc, __bss_endから動的算出) R/W
KAPI_ADDR                 4KB      KernelAPIテーブル (ヒープ末尾直後)         R/W
+4KB                      4KB      ★ SHM前方ガードページ                     NP
+4KB - +260KB             256KB    共有メモリ (IPC用, MEM_SHM_BASE)          R/W
+256KB                    4KB      ★ SHM後方ガードページ                     NP
残り - 0x1FAFFF                    予約 (NP)                                 NP
0x1FB000 - 0x1FBFFF       4KB      ★ カーネルスタックガード                  NP
0x1FC000 - 0x1FFFFC       16KB     カーネルスタック (ESP初期値=0x1FFFFC)      R/W

[ SQLite帯域 (0x200000 - 0x2FFFFF, 1MB) ]
0x200000 - __sqlite_end   ~579KB   SQLite code+BSS                          R/W
__sqlite_end(align) -     128KB    SQLite代替スタック                        R/W
残り - 0x2FFFFF                    カーネル予約域 (NP)                       NP

[ シェル常駐帯域 (0x300000 - 0x3FFFFF, 1MB) ]
0x300000 - +468KB(max)    ~375KB   shell.bin (.text + .data + .bss)           R/W
(BSS終端) - 0x374FFF               newlib sbrk ヒープ (malloc / stdio)        R/W
0x375000 - 0x375FFF       4KB      ★ シェルスタックガード (= sbrk 上限)      NP
0x376000 - 0x37FFFF       40KB     シェルスタック (ESP初期値=0x380000)        R/W
0x380000 - 0x3FFFFF       512KB    シェル exec_heap (KAPI mem_alloc)          R/W

> シェルは newlib の sbrk ヒープと KAPI `mem_alloc` の exec_heap の 2 系統を
> 持つ。かつては両方が BSS 終端から始まり互いを上書きしていた
> (`ls > file` の化け、`pipe: out of memory`、double free 警告)。
> 2026-09-03 に exec_heap を 0x380000 (旧 NP ギャップ) へ分離した。
> PTE に USER は立てないので CPL=3 のアプリからは見えない。

[ ページング (H3b 2026-09-06) ]
恒等マップの守備範囲は 32MB (PAGING_MAP_SIZE、PT 8 枚 = +16KB BSS)。実 RAM として扱うのは
従来どおり 16MB まで (PAGING_RAM_LIMIT: pgalloc / sys_usable_mem_end / ホットデプロイ窓は不変)。
16MB〜32MB は既定 Not-Present で、必要な範囲だけ paging_map_phys() で張る:
0x00F00000 - 0x00F4AFFF          PEGC のリニア窓 (H2、9821 で PEGC 有効時のみ)  supervisor + PCD
0x01000000 - 0x011FFFFF          WAB (Cirrus Xe10) の 2MB リニア窓 (H3b、Cirrus 有効時のみ) supervisor + PCD
  +000000h 表示面 / +04B000h クライアント面 (300KB) / +096000h 塗りパターン

> **デバイス窓の貸し出し規則 (レビュー #5 ②③、2026-09-06)**
> バックエンドが master PD に張る窓は **supervisor + PCD** で、PTE_USER を付けない。
> `paging_addrspace_create()` は master の PDE を 1024 本すべて写すので、master で
> USER にすると CPL=3 アプリが**表示面**に直接書けてしまい、契約 G4 (commit 前の
> 描画は表示面に出ない) が崩れる。CPL=3 に見せるのは **クライアント面だけ** で、
> `gfx_bb_phys_range()` が返す範囲 (Cirrus: リニア窓 +04B000h の 300KB、PEGC/9801:
> 主記憶のバックバッファ) を exec が `paging_addrspace_map_user_keep()` で
> **アプリ PD ごとに** USER へ昇格させる。この 300KB の PTE は共有 PT にあるので
> master からも USER に見えるが、master 側の PDE には USER を伝播させないため
> 実効権限は supervisor のまま (C2 の「共有 + USER」と同じ模型)。
> `_keep` は既存 PTE の **PCD/PWT を引き継ぐ** — 落とすと CPU が書いた画素が
> キャッシュに残り、Cirrus の BLT エンジンが古い VRAM を読む。
> 不変条件はブート時の kselftest (`paging_map_user_keep_selftest`) が毎回検査する。
> 9801 の主記憶バックバッファ (0x6A000、128KB) は選ばれているバックエンドに関わらず
> **常に** USER にする (レビュー #6、2026-09-06): アプリの gfx_init でアクセラレータの
> setup が失敗すると HAL は 9801 へ落ち、以後 `gfx_get_framebuffer()` が 0x6A000 を返す
> ため。写していないとフォールバック直後の最初の描画で #PF になる (`ring3_guard bb` が
> 「書けて生き残る」ことを確認する)。

[ 共有ライブラリ帯域 (0x400000 - 0x4FFFFF, K3 2026-09-06) ]
0x400000 - text_end                libos32gui.shlib の先頭 4KB ジャンプ表 + .text/.rodata  RO, USER, 全 PD 共有
data_vaddr - data_end              .data/.bss (アプリごとに物理ページを複製)         R/W, USER
0x4FFFFF 直下                      .data/.bss の原本 (ロード時に退避)

[ プログラム空間 (0x500000 - mem_end) — 動的レイアウト ]
0x500000 - code_end                .text + .data + .bss (固定上限なし)        R/W
code_end - guard_a                 newlib sbrk (最低 MEM_EXEC_SBRK_MIN=256KB)  R/W
guard_a  (4KB)                     ★ GUARD A: sbrk上限ガード (位置は動的)     NP
guard_a+4KB - heap_top             exec_heap (KAPI mem_alloc)                 R/W
                                   heap_top = CPL=3: アプリ帯上端のスタックガード
                                   直下 (1 枚なら 0x7BF000、2 枚なら 0xBBF000)
                                   / CPL=0: GUARD B - 動的確保リザーブ
                                   大きさ: OS32X ヘッダ heap_size 指定があれば
                                   それ、0 なら空きを sbrk と折半 (2026-09-04)
  ...    - (mem_end-260KB)         (CPL=0 のみ) 動的確保リザーブの穴 1MB
(mem_end-260KB) - (-256KB) 4KB     ★ GUARD B: スタックovrflowガード           NP
(mem_end-256KB) - mem_end  256KB   プログラムスタック (下向き展開)            R/W
(物理末尾) - (+256KB)     256KB   ホットデプロイ・ステージング窓             R/W
                                   (MEM_HOTDEPLOY_SIZE。mem_end はこの分を
                                    差し引いた値 = sys_usable_mem_end())

  ※ 属性: R/W=読み書き可能, R/O=読み取り専用, NP=Not-Present(★はガードページ)
  ※ mem_end は搭載メモリ量からホットデプロイ用の 256KB を引いた値
    (sys_usable_mem_end())。15MB 構成なら 0xF00000 - 0x40000 = 0xEC0000
  ※ カーネル帯域内のKAPI/SHMアドレスは __bss_end を基点に動的算出される
  ※ 入れ子起動は子として走り終了で親へ戻る (最大 4 段)。CPL=3 のプログラムは
    PD ごとに独立したアプリ帯を持つ (09_exec.md)。帯は 0x400000 から 4MB (PDE)
    単位で 1〜2 枚 (tasks/memory/APP_BAND_PDE.md)。0x400000 帯の shlib .text は共有、.data はアプリごと
```

> **0x90000 の自動プレイ観測メールボックス**: ゲーム側が毎フレーム状態ブロックを書き、
> ホストが `GET /api/mem?addr=0x90000&space=phys` で読む。**レイアウトを変えたら
> `game/tools/autoplay/driver.py` の `read_mailbox()` と `EXPORT_VERSION` を同じコミットで直す**
> — 片方だけ変えるとホスト側が黙って古い解釈で読み続ける。

### §2-2 DMA 64KB境界制約

PC-98のDMAコントローラ(8237相当)は16ビットアドレスカウンタとページレジスタを持つ。  
DMA転送が64KB物理アドレス境界 (0x10000, 0x20000, ...) を**またぐ**場合、カウンタがラップアラウンドしてデータが壊れる。

**ルール**: INT 1BhによるFDD読み込みにおいて、`ES:BP`で指定するバッファの開始アドレスから転送バイト数分のアドレスが同じ64KBページ内に収まるようにすること。

```
64KBページ = 物理アドレス >> 16
条件: (start >> 16) == ((start + size - 1) >> 16)

例 NG: 0xFC00 + 8192 = 0x11C00 → ページ0とページ1をまたぐ
例 OK: 0x10000 + 8192 = 0x12000 → ページ1内に収まる
```

### §2-3 セグメント方式によるDMA境界回避

0x10000以降のアドレスへの読み込みにはES:BPセグメント方式を使用する。  
ESを0x200ずつ増加させる (= 物理アドレス +8192) ことで、各読み込みが64KBページ内に安全に収まる。

```
ES=0x1000 → 物理 0x10000 (ページ1先頭, OK)
ES=0x1200 → 物理 0x12000 (ページ1内, OK)
ES=0x1E00 → 物理 0x1E000 (ページ1末尾, 0x1E000+8192=0x20000, ぎりぎり収まる)
ES=0x2000 → 物理 0x20000 (ページ2先頭, OK)
```

---
