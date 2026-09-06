## 第2部 メモリマップ

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
                                   heap_top = CPL=3: 0x7BF000 (帯上端のスタック
                                   ガード直下) / CPL=0: GUARD B - 動的確保リザーブ
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
    PD ごとに独立した 0x500000 帯を持つ (09_exec.md)。0x400000 帯の shlib .text は共有、.data はアプリごと
```

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
