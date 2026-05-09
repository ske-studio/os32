# Phase 2: Device API 2系統化 (CHS / LBA)

## 目的

- Device 構造体に CHS ネイティブ I/O を追加
- 各ドライバの個別 LBA→CHS 変換を除去
- LBA ラッパーを dev.c に集約
- ATAPI CD は LBA のまま維持

## 前提条件

- Phase 1 完了 (loop_dev CHS コア追加済み)

## 依存関係

```
dev.h (構造体変更)
  ← dev.c (LBA ラッパー + ドライバ登録更新)
  ← ide.c (CHS ネイティブ API 新設)
  ← disk.c (CHS API 公開化)
  ← loop_dev.c (blk_read_chs 登録)
  ← iso9660.c (呼び出し側更新)
```

> **⚠️ 警告**: Device 構造体の変更は ABI 破壊を伴う。
> `make clean` → `make all` が必須。
> 途中状態でのテストは不可能。

---

## 変更ファイル一覧

### 1. dev.h — Device 構造体変更

```c
struct _Device {
    const char *name;
    DevType     type;

    /* ジオメトリ (CHS ブロックデバイス用) */
    u16   cyls;         /* シリンダ数 */
    u16   heads;        /* ヘッド数 */
    u16   spt;          /* セクタ/トラック */
    u16   sect_size;    /* バイト/セクタ (旧 sect_size と互換) */
    u32   total_sects;  /* 総セクタ数 (互換用) */

    /* CHS 系統 I/O (FDD, HDD, loop — sect は 1-based) */
    int (*blk_read_chs)(Device *self, u16 cyl, u8 head, u8 sect,
                        void *buf);
    int (*blk_write_chs)(Device *self, u16 cyl, u8 head, u8 sect,
                         const void *buf);

    /* LBA 系統 I/O (ATAPI CD のみ) */
    int (*blk_read)(Device *self, int lba, int count, void *buf);
    int (*blk_write)(Device *self, int lba, int count,
                     const void *buf);

    /* キャラクタデバイス用 */
    int (*chr_read)(Device *self, void *buf, int len);
    int (*chr_write)(Device *self, const void *buf, int len);

    /* デバイス固有制御 */
    int (*ioctl)(Device *self, int cmd, void *arg);
    void *priv;
};

/* 汎用 LBA ラッパー
 * CHS デバイス: ジオメトリから CHS 変換 → blk_read_chs
 * LBA デバイス: blk_read を直接呼び出し */
int dev_blk_read_lba(Device *dev, u32 lba, int count, void *buf);
int dev_blk_write_lba(Device *dev, u32 lba, int count,
                      const void *buf);
```

### フィールド配置の注意

既存の `sect_size` と `total_sects` の位置を維持しつつ、
新規フィールド (`cyls`, `heads`, `spt`) を追加する。

---

### 2. dev.c — 汎用 LBA ラッパー + ドライバ更新

#### 2a. 汎用 LBA ラッパー

```c
int dev_blk_read_lba(Device *dev, u32 lba, int count, void *buf)
{
    int i;
    u8 *p = (u8 *)buf;

    /* LBA ネイティブデバイス (ATAPI CD) */
    if (dev->blk_read)
        return dev->blk_read(dev, (int)lba, count, buf);

    /* CHS デバイス: LBA → CHS 変換 */
    if (!dev->blk_read_chs || dev->spt == 0 || dev->heads == 0)
        return -1;

    for (i = 0; i < count; i++) {
        u32 cur  = lba + (u32)i;
        u8  sect = (u8)((cur % dev->spt) + 1);  /* 1-based */
        u32 tmp  = cur / dev->spt;
        u8  head = (u8)(tmp % dev->heads);
        u16 cyl  = (u16)(tmp / dev->heads);
        int ret  = dev->blk_read_chs(dev, cyl, head, sect, p);
        if (ret != 0) return ret;
        p += dev->sect_size;
    }
    return 0;
}
```

#### 2b. FDD ドライバ (fd0/fd1)

```c
/* 変更前 */
static int fdd0_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return disk_read_lba(0, lba, count, buf);
}

/* 変更後 */
static int fdd0_read_chs(Device *self, u16 cyl, u8 head, u8 sect,
                          void *buf)
{
    (void)self;
    return fdc_read_sector(0, (int)cyl, (int)head, (int)sect, buf);
}

static Device fdd0_dev = {
    "fd0", DEV_BLOCK,
    .cyls = 77, .heads = 2, .spt = 8,
    .sect_size = 1024, .total_sects = 1232,
    .blk_read_chs = fdd0_read_chs,
    .blk_write_chs = fdd0_write_chs,
    .blk_read = 0,     /* CHS デバイスなので NULL */
    .blk_write = 0,
    ...
};
```

> **注意**: C89 では designated initializer が使えない。
> 従来通りの位置ベース初期化を使用する。

#### 2c. HDD ドライバ (hd0-3)

```c
/* 変更前 */
static int hd0_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return ide_read_sectors(0, lba, count, buf);
}

/* 変更後 */
static int hd0_read_chs(Device *self, u16 cyl, u8 head, u8 sect,
                         void *buf)
{
    (void)self;
    return ide_read_sector_chs(0, cyl, head, sect, buf);
}
```

#### 2d. CD ドライバ (cd0) — 変更なし

```c
/* ATAPI CD は LBA のまま */
static int cd0_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return atapi_read_sectors(0, (u32)lba, (u32)count, buf);
}

static Device cd0_dev = {
    "cd0", DEV_BLOCK,
    .cyls = 0, .heads = 0, .spt = 0,  /* CHS ジオメトリなし */
    .sect_size = 2048, .total_sects = ...,
    .blk_read_chs = 0,    /* CHS 非対応 */
    .blk_write_chs = 0,
    .blk_read = cd0_read,  /* LBA ネイティブ */
    .blk_write = 0,
    ...
};
```

---

### 3. ide.c — CHS ネイティブ API 新設

```c
/* 変更前: LBA を受け取り内部で CHS 変換 */
int ide_read_sector(int drive, u32 lba, void *buf);

/* 変更後: CHS を直接受け取る */
int ide_read_sector_chs(int drive, u16 cyl, u8 head, u8 sect,
                        void *buf);
int ide_write_sector_chs(int drive, u16 cyl, u8 head, u8 sect,
                         const void *buf);

/* 旧 API は互換のため残すか、削除するかは Phase 4 で決定 */
```

**ide_set_chs の変更:**
```c
/* 変更前: LBA → CHS 変換を内包 */
static void ide_set_chs(int drive, u32 lba, u8 count)

/* 変更後: CHS を直接受け取る */
static void ide_set_chs_regs(int drive, u16 cyl, u8 head, u8 sect,
                              u8 count)
{
    outp(IDE_SECT_CNT, (unsigned)count);
    outp(IDE_SECT_NUM, (unsigned)sect);
    outp(IDE_CYL_LO,   (unsigned)(cyl & 0xFF));
    outp(IDE_CYL_HI,   (unsigned)((cyl >> 8) & 0xFF));
    outp(IDE_DRV_HEAD,
         (unsigned)(IDE_DRV_SEL_CHS | (head & 0x0F)
                    | ((drive % 2) ? 0x10 : 0x00)));
}
```

---

### 4. disk.c — CHS API 公開化

```c
/* 変更前: static (内部のみ) */
static int disk_read_chs(int drv, int cyl, int head, int sect,
                          int count, void *buf)

/* 変更後: 公開 (1セクタ版) */
int disk_read_chs_1(int drv, int cyl, int head, int sect, void *buf)
{
    return fdc_read_sector(drv, cyl, head, sect, buf);
}

/* disk_read_lba / disk_write_lba は削除候補 (Phase 4) */
```

---

### 5. loop_dev.c — Device 登録の CHS 化

```c
/* D88 アタッチ時 */
s->dev.blk_read_chs  = loop_blk_read_chs;  /* 新 */
s->dev.blk_write_chs = 0;
s->dev.blk_read      = 0;   /* LBA 版は NULL */
s->dev.blk_write     = 0;
s->dev.cyls  = s->cyls;
s->dev.heads = s->heads;
s->dev.spt   = s->spt;
s->dev.sect_size = s->bps;

/* Device 経由の CHS 読み出し */
static int loop_blk_read_chs(Device *self, u16 cyl, u8 head,
                              u8 sect, void *buf)
{
    LoopSlot *s = (LoopSlot *)self->priv;
    int slot = (int)(s - slots);
    return loop_dev_read_chs(slot, (u8)cyl, head, sect, buf);
}
```

---

### 6. iso9660.c — 呼び出し側更新

```c
/* 変更前 */
static int iso_read_sector(Iso9660Ctx *ctx, u32 lba, void *buf)
{
    Device *dev = dev_find("cd0");
    if (!dev || !dev->blk_read) return VFS_ERR_IO;
    return dev->blk_read(dev, (int)lba, 1, buf);
}

/* 変更後 */
static int iso_read_sector(Iso9660Ctx *ctx, u32 lba, void *buf)
{
    Device *dev = dev_find("cd0");
    if (!dev) return VFS_ERR_IO;
    return dev_blk_read_lba(dev, lba, 1, buf);
}
```

---

## テスト計画

### 全デバイス回帰テスト (必須)

| # | テスト | デバイス | 期待結果 |
|---|---|---|---|
| 1 | `cat /etc/profile` | hd0 (IDE) | ファイル内容表示 |
| 2 | HDD 書き込み + 読み戻し | hd0 | 一致 |
| 3 | `losetup /dos5_1.fdi 0` + dd | lo0 (FDI) | ground truth 一致 |
| 4 | `losetup /Ys.D88 0` + dd noerr | lo0 (D88) | 630,784B + エラー報告 |
| 5 | `mount cd0 /cdrom` + `ls /cdrom` | cd0 (ATAPI) | ファイル一覧表示 |
| 6 | v86 D88 ブート | v86 + loop | Ys ブート成功 |

### 構造体検証

| # | 確認事項 |
|---|---|
| 1 | `sizeof(Device)` がビルドログで確認可能か |
| 2 | 全 Device 初期化で新フィールド (`cyls/heads/spt`) が正しいか |
| 3 | cd0 で `blk_read_chs == NULL` かつ `blk_read != NULL` |

## コミット戦略

```
refactor: dev.h: add CHS I/O to Device struct (dual CHS/LBA)
refactor: dev.c: implement dev_blk_read_lba wrapper
refactor: ide.c: add ide_read_sector_chs (remove LBA→CHS)
refactor: disk.c: expose disk_read_chs_1
refactor: dev.c: update fd0/fd1/hd0-3 to use blk_read_chs
refactor: loop_dev.c: register blk_read_chs instead of blk_read
refactor: iso9660.c: use dev_blk_read_lba
```
