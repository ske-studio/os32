# Phase A: loop_dev フォーマット拡張

## 目的

`loop_dev.c` に HDI / RAW / WRITE サポートを追加し、
V86 サブシステムのディスクバックエンドとして完全に機能させる。

---

## A-1. HDI (Anex86 HDD イメージ) サポート追加

### 対象ファイル

- `drivers/loop_dev.c`
- `drivers/loop_dev.h`

### HDI ヘッダ仕様

PC-98 Anex86 HDI = **4096 バイトヘッダ + RAW データ**。
FDI と同じレイアウトだが、用途が HDD。

```
offset 0x00: reserved (通常 0)
offset 0x04: reserved (通常 0)
offset 0x08: hdr_size    (u32 LE, = 0x1000 = 4096)
offset 0x0C: data_size   (u32 LE, = file_size - hdr_size)
offset 0x10: sect_size   (u32 LE, 通常 512)
offset 0x14: spt         (u32 LE, 例: 17)
offset 0x18: surfaces    (u32 LE, 例: 4-16)
offset 0x1C: cyls        (u32 LE, 例: 615)
```

### フォーマット判別の大原則 — 拡張子を信じる

D88 / FDI / HDI のいずれも **ヘッダ先頭にマジックナンバーを持たない**。

| フォーマット | offset 0x00 の内容 | マジック |
|-------------|-------------------|---------|
| D88 | ディスク名 (ASCII 17B, 任意文字列) | ❌ なし |
| FDI | 予約 (通常 0x00000000) | ❌ なし |
| HDI | 予約 (通常 0x00000000) | ❌ なし |

さらに FDI と HDI は **ヘッダ構造が完全に同一** (4096B ヘッダ + RAW データ) であり、
ヘッダ内容だけから確実に区別することは不可能。

したがって **拡張子を第一判別基準** とし、
ヘッダのバリデーションは整合性チェック (壊れていないかの確認) のみに使う。

### 判別ロジック (拡張子優先)

```
1. 拡張子で判別 (detect_format):
   .d88 / .d77 / .88d → LOOP_FMT_D88
   .fdi               → LOOP_FMT_FDI
   .hdi               → LOOP_FMT_HDI   ← 新規
   .img / .bin / その他 → LOOP_FMT_RAW  ← 新規
   拡張子なし / 不明    → LOOP_FMT_NONE (エラー)

2. ヘッダ構造バリデーション (attach_* 内部):
   拡張子で選択した attach_d88/attach_fdi/attach_hdi を呼び出す。
   ヘッダ整合性チェックに失敗 → -2 を返す (エラー)。
```

> **ヒューリスティック自動判別は行わない**。
> 呼び出し側 (v86_session / shell) がパス文字列を渡し、
> `detect_format` が拡張子で LOOP_FMT_* を返す。

### detect_format の拡張

```c
static int detect_format(const char *path)
{
    if (str_ends_with(path, ".d88") ||
        str_ends_with(path, ".d77") ||
        str_ends_with(path, ".88d"))
        return LOOP_FMT_D88;
    if (str_ends_with(path, ".fdi"))
        return LOOP_FMT_FDI;
    if (str_ends_with(path, ".hdi"))
        return LOOP_FMT_HDI;
    if (str_ends_with(path, ".img") ||
        str_ends_with(path, ".bin"))
        return LOOP_FMT_RAW;
    return LOOP_FMT_NONE;
}
```

### loop_dev_attach_fd — fmt 引数の追加

現在の `loop_dev_attach_fd(int fd, int slot)` はヘッダ解析で
フォーマットを自動判別しているが、拡張子情報を受け取れない。
**fmt 引数を追加** し、パスベース `loop_dev_attach` から拡張子判定結果を伝搬する。

```c
/* 新シグネチャ */
int loop_dev_attach_fd(int fd, int slot, int fmt);

/* loop_dev_attach から呼ぶ */
int loop_dev_attach(const char *vfs_path, int slot)
{
    int fmt = detect_format(vfs_path);
    if (fmt == LOOP_FMT_NONE)
        return -2;
    fd = vfs_open(vfs_path, 0);
    if (fd < 0) return -1;
    ret = loop_dev_attach_fd(fd, slot, fmt);
    /* ... */
}
```

`loop_dev_attach_fd` 内部では `fmt` に応じて `attach_d88` / `attach_fdi` /
`attach_hdi` / `attach_raw` を直接呼び出す:

```c
int loop_dev_attach_fd(int fd, int slot, int fmt)
{
    LoopSlot *s = &loop_slots[slot];
    u32 fsize;
    int ret;

    /* slot/fd バリデーション省略 */
    fsize = vfs_file_size(fd);

    switch (fmt) {
    case LOOP_FMT_D88:  ret = attach_d88(s, fd, fsize); break;
    case LOOP_FMT_FDI:  ret = attach_fdi(s, fd, fsize); break;
    case LOOP_FMT_HDI:  ret = attach_hdi(s, fd, fsize); break;
    case LOOP_FMT_RAW:  ret = attach_raw(s, fd, fsize); break;
    default:            return -2;
    }
    if (ret != 0) return ret;

    s->fd      = fd;
    s->in_use  = 1;
    s->owns_fd = 0;
    return 0;
}
```

> **v86_session.c への影響**: `v86_open_image_to_loop()` (Phase C-1) では
> パス文字列を持っているため `loop_dev_attach(path, slot)` 経由で呼べばよい。
> `loop_dev_attach_fd` を直接呼ぶ既存箇所は `v86_disk_set_d88` /
> `v86_disk_set_file` のみ (Phase B で置換対象)。

### 実装: attach_hdi

```c
/* フォーマット種別に追加 */
#define LOOP_FMT_HDI      3

/* attach_hdi: attach_fdi と同一構造 (ヘッダバリデーションのみ異なる) */
static int attach_hdi(LoopSlot *s, int fd, u32 fsize)
{
    u8  hdr[32];
    u32 hdr_size, data_size, sect_size, spt, surfaces, cyls;
    int rd;

    if (fsize < 4096) return -2;

    vfs_seek(fd, 0, 0);
    rd = vfs_read_fd(fd, hdr, 32);
    if (rd < 32) return -4;

    hdr_size  = le32(hdr + 8);
    data_size = le32(hdr + 12);
    sect_size = le32(hdr + 16);
    spt       = le32(hdr + 20);
    surfaces  = le32(hdr + 24);
    cyls      = le32(hdr + 28);

    /* バリデーション */
    if (hdr_size < 32 || hdr_size > 65536) return -2;
    if (sect_size == 0 || sect_size > 4096) return -2;
    if (spt == 0 || spt > 255) return -2;
    if (surfaces == 0 || surfaces > 255) return -2;
    if (cyls == 0 || cyls > 65535) return -2;
    if (hdr_size + data_size > fsize) return -2;

    s->data_offset   = hdr_size;
    s->file_size     = fsize;
    s->cyls          = (u16)cyls;
    s->heads         = (u8)surfaces;
    s->spt           = (u8)spt;
    s->lba_sect_size = (u16)sect_size;
    s->total_lba     = cyls * surfaces * spt;
    s->fmt           = LOOP_FMT_HDI;
    s->dev.blk_read  = raw_blk_read;
    return 0;
}
```

> **注意**: `attach_hdi` では `surfaces >= 3` の制約は **設けない**。
> 拡張子 `.hdi` を信じるため、ヘッダ内容が FDI と重複しても問題ない。

### LoopSlot.cyls の u16 拡張

HDI では `cyls = 615` 等の値が一般的であり `u8` (max 255) では収まらない。
`LoopSlot.cyls` を `u16` に拡張する。影響範囲:

- `LoopSlot.cyls` の型変更 (`u8` → `u16`)
- `loop_dev_get_geometry` の `cyls` 引数 (`u8*` → `u16*`)
- `loop_dev_read_chs` の `cyl` 引数 (`u8` → `u16`)
- `loop_blk_read_chs` (Device 経由)
- `dev.h` の `Device.cyls` は既に `u16` (変更不要)

---

## A-2. RAW フォーマット追加

### 対象ファイル

- `drivers/loop_dev.c`

### フォーマット定数

```c
#define LOOP_FMT_RAW      4
```

### attach_raw ロジック

ファイルサイズから既知容量テーブルで C/H/S/BPS を推定する。
**未知サイズの場合は拒否する** (誤判定防止)。

```c
static int attach_raw(LoopSlot *s, int fd, u32 fsize)
{
    /* 既知容量テーブル */
    struct { u32 size; u8 cyls; u8 heads; u8 spt; u16 bps; } known[] = {
        { 1261568, 77, 2, 8,  1024 }, /* 2HD 1232KB (PC-98) */
        {  655360, 80, 2, 8,   512 }, /* 2DD 640KB */
        {  737280, 80, 2, 9,   512 }, /* 2DD 720KB */
        { 1474560, 80, 2, 18,  512 }, /* 2HD 1.44MB (IBM) */
        {  327680, 40, 2, 8,   512 }, /* 2D 320KB */
        {  163840, 40, 1, 8,   256 }, /* 1D 160KB */
        { 0, 0, 0, 0, 0 }
    };
    int i;

    for (i = 0; known[i].size != 0; i++) {
        if (fsize == known[i].size) {
            s->data_offset   = 0;
            s->file_size     = fsize;
            s->cyls          = known[i].cyls;
            s->heads         = known[i].heads;
            s->spt           = known[i].spt;
            s->lba_sect_size = known[i].bps;
            s->total_lba     = (u32)s->cyls * s->heads * s->spt;
            s->fmt           = LOOP_FMT_RAW;
            s->dev.blk_read  = raw_blk_read;
            return 0;
        }
    }
    return -2;  /* 未知サイズ: 拒否 */
}
```

> **設計判断**: 任意サイズの RAW ファイルを自動で受け入れると、
> テキストファイル等が誤って RAW ディスクイメージとして解釈されるリスクがある。
> 既知容量テーブルとの完全一致のみ許可する。
> 必要であれば将来 `loop_dev_attach_raw_explicit(fd, slot, &geom)` を追加。

### loop_dev_attach_fd への統合

```c
/* D88/FDI/HDI いずれにも該当しない場合 */
if (fmt == LOOP_FMT_NONE) {
    fmt = LOOP_FMT_RAW;  /* attach_raw 内で既知容量チェック */
}
```

---

## A-3. WRITE 経路追加

### 対象ファイル

- `drivers/loop_dev.c`
- `drivers/loop_dev.h`

### 新規関数

```c
int loop_dev_write_chs(int slot, u8 cyl, u8 head, u8 sect, const void *buf)
```

`loop_dev_read_chs` と対称構造:

- **D88**: `loop_dev_seek_d88` でセクタデータ位置を取得 → `vfs_write_fd`
- **FDI/HDI/RAW**: CHS → オフセット計算 → `vfs_seek` + `vfs_write_fd`

### 実装ポイント

`v86_disk.c` の既存 D88 WRITE 実装 (L771-810) をそのまま移植元にする。
現在のコード:

```c
/* v86_disk.c L781-792 (D88 WRITE) */
u32 data_off = loop_dev_seek_d88(
    v86_loop_slot,
    wc_trk_c, wc_trk_h,
    wc_id_c, wc_id_h, wc_r,
    &sec_data_len, &trk_spt);
if (data_off == 0) { /* error */ }
vfs_seek(fdd_fd, data_off, 0);
vfs_write_fd(fdd_fd, wr_buf, wr_chunk);
```

これを `loop_dev_write_chs` に集約し、`v86_disk.c` からは呼ぶだけにする。

---

## A-4. 新規公開 API

### 対象ファイル

- `drivers/loop_dev.h`
- `drivers/loop_dev.c`

### 追加する関数・定数

```c
/* メディア種別定数 */
#define LOOP_MEDIA_2HD_1232  0x90
#define LOOP_MEDIA_2DD       0x10
#define LOOP_MEDIA_2HD_144   0x30
#define LOOP_MEDIA_HDD       0x80

/* 新規公開関数 */
int  loop_dev_get_media(int slot);
int  loop_dev_is_hdd(int slot);
u8   loop_dev_get_sec_n(int slot);   /* セクタ長コード N (log2(bps/128)) */
int  loop_dev_get_format(int slot);  /* LOOP_FMT_* */
int  loop_dev_write_chs(int slot, u8 cyl, u8 head, u8 sect, const void *buf);
```

### `loop_dev_get_sec_n` 実装

```c
u8 loop_dev_get_sec_n(int slot)
{
    LoopSlot *s;
    u16 tmp;
    u8 n;

    if (slot < 0 || slot >= LOOP_MAX_SLOTS) return 0;
    s = &loop_slots[slot];
    if (!s->in_use) return 0;

    tmp = s->lba_sect_size;
    n = 0;
    while (tmp > 128 && n < 8) { tmp >>= 1; n++; }
    return n;
}
```

### `loop_dev_is_hdd` 実装

```c
int loop_dev_is_hdd(int slot)
{
    if (slot < 0 || slot >= LOOP_MAX_SLOTS) return 0;
    return (loop_slots[slot].fmt == LOOP_FMT_HDI) ? 1 : 0;
}
```

---

## A-5. 検証

| 項目 | 手順 | 期待結果 |
|------|------|----------|
| D88 回帰 | `loop attach Ys.D88 lo0` → `loop status` | C=77 H=2 SPT=8 BPS=1024 |
| FDI 回帰 | `loop attach DISK.FDI lo0` → `loop status` | ヘッダのジオメトリ通り |
| HDI 新規 | `loop attach disk.hdi lo0` → `loop status` | C/H/S が HDI ヘッダ通り |
| RAW 新規 | (1261568B ファイル作成) → `loop attach` | C=77 H=2 SPT=8 BPS=1024 |
| ビルド | `make all` | エラー 0 |
| 起動 | NP21/W で `vdos -native Ys.D88` | タイトル画面到達 |
