# 4. メモリ管理 — バッキングRAM・ページング・IVT/BDA

ソース: [`v86_mem.c`](../../../kernel/v86_mem.c), [`v86_bda.h`](../../../kernel/v86_bda.h)

## 4.1 バッキングRAM

V86ゲストのコンベンショナルメモリ (0x00000-0x9FFFF, 640KB) は、
OS32カーネルのメモリ空間と物理的に分離する必要がある。

### 確保方式

`pgalloc_alloc_n(160)` で連続160ページ (640KB) を動的確保。
確保先はプログラム空間 (0x400000+) であり、カーネル領域とは物理的に分離。

### ページテーブルリマップ

仮想 0x00000-0x8EFFF → 物理 backing+0 〜 backing+0x8EFFF にリマップ。
これにより、V86ゲストが仮想アドレス 0x0000 にアクセスすると、
バッキングRAMの対応位置にアクセスされる。

### カーネルスタック分離 (0x8F000-0x9FFFF)

OS32のカーネルスタック (物理 0x8F000-0x9FFFF) は、V86ゲストからアクセス
されないよう分離する必要がある。

```
[setup 時]
1. 物理 0x8F000-0x9FFFF の内容を backing+0x8F000 にコピー
2. 仮想 0x8F000-0x9FFFF → 物理 backing+0x8F000 にリマップ
→ カーネルはコピー上で動作、V86はバッキングRAMにアクセス

[teardown 時]
1. 仮想 0x8F000-0x9FFFF → 物理 0x8F000 にリマップ (アイデンティティ)
2. backing+0x8F000 の内容を物理 0x8F000 にコピーバック
→ カーネルスタックが元の物理ページに復元される
```

> ⚠️ teardown時の順序が重要: 先にリマップ → 後にコピー。
> 逆順だと両方同じ物理ページを指してしまいno-opになる。

## 4.2 メモリマップ詳細

| 仮想アドレス | 物理マッピング | PTE属性 | 用途 |
|-------------|---------------|---------|------|
| 0x00000-0x003FF | backing+0x000 | PRESENT\|RW\|USER | IVT (256ベクタ × 4バイト) |
| 0x00400-0x005FF | backing+0x400 | PRESENT\|RW\|USER | BDA (BIOS Data Area) |
| 0x00600-0x8EFFF | backing+0x600 | PRESENT\|RW\|USER | ゲスト使用可能メモリ |
| 0x8F000-0x9FFFF | backing+0x8F000 | PRESENT\|RW\|USER | スタック分離領域 |
| 0xA0000-0xA1FFF | 0xA0000 | PRESENT\|RW\|USER | TVRAM (文字コード) |
| 0xA2000-0xA3FFF | 0xA2000 | PRESENT\|RW\|USER | TVRAM (アトリビュート) |
| 0xA4000-0xA7FFF | 0xA4000 | PRESENT\|USER | CGウィンドウ (R/O) |
| 0xA8000-0xBFFFF | 0xA8000 | PRESENT\|RW\|USER | GVRAM Plane B/R/G (96KB) |
| 0xC0000-0xDFFFF | 0xC0000 | PRESENT\|USER | 拡張ROM BIOS (R/O) |
| 0xE0000-0xE7FFF | 0xE0000 | PRESENT\|RW\|USER | GVRAM Plane E (32KB) |
| 0xE8000-0xEFFFF | 0xE8000 | PRESENT\|USER | 拡張ROM/バンクメモリ (R/O) |
| 0xF0000-0xFFFFF | 0xF0000 | PRESENT\|USER | BIOS ROM (R/O) |

> 全エントリに `PTE_USER` が必要。V86モードはCPL=3であり、U/Sビットなしでは
> ページフォルト (#PF) が発生する。PDE[0] にも `PTE_USER` を設定。

## 4.3 IVT (割り込みベクタテーブル) 構築

バッキングRAMの先頭 1KB (256エントリ × 4バイト) に IVT を構築。

### ダミーIRETハンドラ

全256ベクタを `0x003F:0x0000` (リニア 0x3F0) に向ける。
リニア 0x3F0 にIRET命令 (0xCF) を1バイト配置。

```
IVT[0x00] = 0x003F:0x0000  → IRET
IVT[0x01] = 0x003F:0x0000  → IRET
...
IVT[0xFF] = 0x003F:0x0000  → IRET
```

> 0x3F0 = INT FCh のベクタ位置。IVT自体の末尾にIRETを配置することで
> 追加メモリを消費せず、ゲストが上書きしない限り安全。

### ゲストによるIVT更新

DOSやゲームのIPL/ドライバがINTベクタをフックする場合、ゲストは
バッキングRAM上のIVTを直接書き換える。GPハンドラは毎回IVTを参照するため、
ゲストのフック内容が自動的に反映される。

## 4.4 BDA (BIOS Data Area) 初期値

バッキングRAMの 0x0400-0x05FF に、最低限必要な値を設定。

| オフセット | サイズ | 値 | 意味 |
|-----------|-------|-----|------|
| 0x0400 | BYTE | 0x00 | BIOS_FLAG2: 機種フラグ |
| 0x0413 | WORD | 640 | MEM_SIZE: コンベンショナルメモリ (KB) |
| 0x0480 | BYTE | 0x00 | CPU_FLAG: V33A=0 |
| 0x0484 | BYTE | 0x03 | CPU_TYPE: i386以上 |
| 0x0495 | BYTE | 0x00 | GRAPH_CHG: GRCG OFF |
| 0x0501 | BYTE | 0x24 | BIOS_FLAG: bit5=1(非無印), bit2=1(640KB) |
| 0x0502-0x0522 | 32B | — | キーボードバッファ (16エントリ × 2バイト) |
| 0x0524 | WORD | 0x0502 | KB_HEAD: 取出ポインタ |
| 0x0526 | WORD | 0x0502 | KB_TAIL: 入力ポインタ |
| 0x0528 | BYTE | 0x00 | KB_COUNT: バッファ内キー数 |
| 0x053C | BYTE | 0x12 | CRT_STS: bit4=16色, bit1=GRCG搭載 |
| 0x055C | WORD | 0x0001 | DISK_EQUIP: FDD UNIT#0のみ接続 |
| 0x055D | BYTE | 0x00 | SASI/IDE: HDDなし |
| 0x0564 | 8B | — | FDC結果バッファ (INT 1Bh更新) |
| 0x0584 | BYTE | 0x90 | BOOT_DEV: 1MB FDD UNIT#0 |
| 0x05AE | BYTE | 0xA0 | CONV_MEM: 640KB (×4KB単位) |

## 4.5 TVRAMメモリスイッチ

PC-98の TVRAM 0xA0000 + 0x3FE2-0x3FF7 にメモリスイッチ (MEMSW) がある。
FreeDOS(98) の `init_crt` がこの領域を参照するため、初期化が必要。

| アドレス | 名前 | 値 | 意味 |
|---------|------|-----|------|
| 0xA3FE2 (WORD) | MEMSW1 | 0x48 | bit6=25行モード, bit3=RS232C |
| 0xA3FE4 (WORD) | MEMSW2 | 0x01 | 10MHzクロック系 |
| 0xA3FE6 (WORD) | MEMSW3 | 0x04 | bit2=31kHz CRT |

## 4.6 画面初期化

`v86_mem_setup()` の末尾で、GDC/GVRAM/パレットを初期化:

1. GVRAM全面クリア (Plane B/R/G: 96KB + Plane E: 32KB)
2. TVRAMクリア (文字コード=空白, アトリビュート=白)
3. GRCG OFF
4. アナログパレット初期化 (PC-98デフォルト16色)
5. テキスト画面表示OFF → グラフィック画面表示ON
6. グラフィックGDC SCROLL/PITCH/CSRFORM 初期化
7. テキストGDC SCROLL/PITCH 初期化
8. ページフリッピング解除 (ページ0)
9. モードフリップフロップ2初期化 (16色, GDC 2.5MHz)

## 4.7 v86_phys_addr() — アドレス変換

V86の seg:off をカーネルがアクセスできるリニアアドレスに変換する関数。

```c
u8 *v86_phys_addr(u32 seg, u32 off) {
    u32 linear = (seg << 4) + off;
    linear &= 0xFFFFF;  /* 1MBラップ */
    if (v86_backing_enabled && linear < 0xA0000UL)
        return (u8 *)(v86_backing_phys + linear);
    return (u8 *)linear;
}
```

0x00000-0x9FFFF → バッキングRAM経由、0xA0000-0xFFFFF → 実物理アドレス。

## 4.8 TVRAM退避・復元

V86開始前にOS32のテキスト画面 (物理 0xA0000-0xA1FFF, 8KB) を
静的バッファに退避し、V86終了後に復元する。

```
v86_tvram_save()   → 物理 0xA0000 → tvram_save_buf[8KB]
v86_tvram_restore() → tvram_save_buf[8KB] → 物理 0xA0000
```
