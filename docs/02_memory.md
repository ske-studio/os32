## 第2部 メモリマップ

### §2-1 物理メモリ配置

```
アドレス範囲              サイズ    用途                                    属性
─────────────────────────────────────────────────────────────────────────────
[ コンベンショナルメモリ (0x00000 - 0xFFFFF) ]
0x00000 - 0x00FFF         4KB      NP (NULLポインタ検出, ブート後)           NP
0x01000 - 0x49FFF         ~292KB   フォントキャッシュ (kcg.c, ブート後配置)   R/W
0x4A000 - 0x69FFF         128KB    Unicode-JIS変換テーブル (utf8.c)          R/W
0x6A000 - 0x89FFF         128KB    GFXバックバッファ (32KB × 4プレーン)      R/W
0x8A000 - 0x8EFFF         20KB     空き (将来用)                            R/W
0x8F000 - 0x8FFFF         4KB      ★ カーネルスタックガード                  NP
0x90000 - 0x9FFFF         64KB     カーネルスタック (ESP初期値=0x9FFFC)       R/W
0xA0000 - 0xEFFFF         320KB    VRAM (テキスト + グラフィック)             R/W
0xF0000 - 0xFFFFF         64KB     BIOS ROM                                  R/O

[ カーネル帯域 (0x100000 - 0x1FFFFF, 1MB) — 動的レイアウト ]
0x100000 - __bss_end      ~200KB   カーネルバイナリ (.text + .data + .bss)    R/W
KHEAP_BASE - +320KB       320KB    カーネルヒープ (kmalloc, __bss_endから動的算出) R/W
KAPI_ADDR                 4KB      KernelAPIテーブル (ヒープ末尾直後)         R/W
+4KB                      4KB      ★ SHM前方ガードページ                     NP
+4KB - +260KB             256KB    共有メモリ (IPC用, MEM_SHM_BASE)          R/W
+256KB                    4KB      ★ SHM後方ガードページ                     NP
残り - 0x1FFFFF                    予約 (NP)                                 NP

[ SQLite帯域 (0x200000 - 0x2FFFFF, 1MB) ]
0x200000 - __sqlite_end   ~579KB   SQLite code+BSS                          R/W
__sqlite_end(align) -     128KB    SQLite代替スタック                        R/W
残り - 0x2FFFFF                    カーネル予約域 (NP)                       NP

[ シェル常駐帯域 (0x300000 - 0x3FFFFF, 1MB) ]
0x300000 - +468KB(max)    ~113KB   shell.bin (.text + .data + .bss)           R/W
0x375000 - 0x375FFF       4KB      ★ シェルスタックガード                    NP
0x376000 - 0x37FFFF       40KB     シェルスタック (ESP初期値=0x380000)        R/W
0x380000 - 0x3FFFFF       512KB    帯域間ギャップ (NP)                       NP

[ プログラム空間 (0x400000 - mem_end) — 動的レイアウト ]
0x400000 - 0x4FFFFF       1MB      .text + .data + .bss + sbrk (最大1MB)      R/W
0x500000 - 0x500FFF       4KB      ★ GUARD A: sbrk上限ガード                 NP
0x501000 - ...                     exec_heap (動的確保上限まで)               R/W
  ...    - (mem_end-260KB)         ↓
(mem_end-260KB) - (-256KB) 4KB     ★ GUARD B: スタックovrflowガード           NP
(mem_end-256KB) - mem_end  256KB   プログラムスタック (下向き展開)            R/W

  ※ 属性: R/W=読み書き可能, R/O=読み取り専用, NP=Not-Present(★はガードページ)
  ※ mem_end は搭載メモリ量 (14MB構成なら mem_end=0xE00000)
  ※ カーネル帯域内のKAPI/SHMアドレスは __bss_end を基点に動的算出される
  ※ プログラムの入れ子（ネスト）起動時はプロセスが完全に破棄・「置換」される
    ため、プログラム空間は1つのみ利用します。
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
