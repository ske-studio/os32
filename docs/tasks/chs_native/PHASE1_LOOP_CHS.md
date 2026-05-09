# Phase 1: loop_dev CHS コア + v86_disk 統合

## 目的

- loop_dev.c に CHS レベルの公開 API を追加
- v86_disk.c の重複 D88 処理コードを削除 (~200行)
- v86_session.c のフォーマット判定重複を解消 (~165行)
- 合計 **~365行** のコード削除

## 前提条件

- なし (Phase 1 は独立して実行可能)

## 依存関係

```
loop_dev.c (CHS コア追加)
  ← v86_disk.c (重複コード削除、loop_dev API 呼び出し)
  ← v86_session.c (フォーマット判定を loop_dev_attach_fd に統合)
```

---

## 変更ファイル一覧

### 1. loop_dev.h — CHS 公開 API 追加

**追加する関数:**

```c
/* fd ベースのアタッチ (フォーマット自動判別)
 * 既に vfs_open 済みの fd を直接スロットにアタッチする。
 * ファイルヘッダを読み取り、D88/FDI/RAW を自動判別。
 * 戻り値: 0=成功, -1=スロット不正, -2=フォーマット不正,
 *         -3=スロット使用中, -4=I/Oエラー */
int loop_dev_attach_fd(int fd, int slot);

/* CHS セクタ読み出し (全フォーマット共通)
 * D88: トラックテーブル走査 → セクタヘッダ照合 → データ読み出し
 * FDI/RAW: オフセット計算 → vfs_read
 * sect は 1-based (ATA/FDC 準拠)
 * 戻り値: 0=成功, -1=セクタ不在/エラー */
int loop_dev_read_chs(int slot, u8 cyl, u8 head, u8 sect, void *buf);

/* D88 専用: FDC エミュレーション用 CHS 検索
 *
 * µPD765A の READ DATA コマンドをエミュレートする。
 * トラック選択 (trk_c/trk_h): SEEK で設定された物理位置
 * セクタID照合 (id_c/id_h/id_r): コマンドの論理 C/H/R
 *
 * Ys 等のコピープロテクションでは:
 *   SEEK cyl=1 → READ C=0, H=1, R=1
 * のように物理トラックと論理IDが異なる。
 *
 * 戻り値: セクタデータのファイルオフセット (0=未発見)
 * out_data_len: セクタデータ長 (128<<N)
 * out_spt: トラック内セクタ数 */
u32 loop_dev_seek_d88(int slot,
                      u8 trk_cyl, u8 trk_head,
                      u8 id_c, u8 id_h, u8 id_r,
                      u32 *out_data_len, u16 *out_spt);

/* スロットの VFS fd を取得 (-1 = 未アタッチ) */
int loop_dev_get_fd(int slot);

/* ジオメトリ取得
 * 戻り値: 0=成功, -1=未アタッチ */
int loop_dev_get_geometry(int slot, u8 *cyls, u8 *heads, u8 *spt,
                          u16 *bps, u32 *total_lba);
```

---

### 2. loop_dev.c — 実装変更

#### 2a. loop_dev_attach のリファクタリング

**現在の構造:**
```c
int loop_dev_attach(const char *vfs_path, int slot)
{
    fd = vfs_open(path, 0);
    /* ヘッダ読み取り → フォーマット判別 → LoopSlot 設定 */
}
```

**変更後の構造:**
```c
/* 内部コア: fd からフォーマット判別 + スロット設定 */
int loop_dev_attach_fd(int fd, int slot)
{
    /* ヘッダ読み取り → フォーマット判別 → LoopSlot 設定 */
    /* fd の所有権は呼び出し側に残る (close しない) */
}

/* パスベースのアタッチ (既存 API 互換) */
int loop_dev_attach(const char *vfs_path, int slot)
{
    fd = vfs_open(path, 0);
    ret = loop_dev_attach_fd(fd, slot);
    if (ret != 0) { vfs_close(fd); return ret; }
    s->owns_fd = 1;  /* パス経由の場合は detach 時に close */
    return 0;
}
```

> **注意**: `loop_dev_attach_fd` 経由の場合、fd は呼び出し側が所有する。
> `loop_dev_detach` では `owns_fd` フラグに応じて close するかを決定。

#### 2b. LoopSlot 構造体変更

```c
typedef struct {
    /* 既存フィールド */
    int      in_use;
    int      fd;
    int      is_d88;
    u32      data_offset;   /* FDI/RAW: データ開始オフセット */
    u32      data_size;
    u8       cyls, heads, spt;
    u16      bps;
    u32      total_lba;
    u32      track_offset[164]; /* D88 トラックテーブル */
    Device   dev;

    /* 追加フィールド */
    int      owns_fd;       /* 1=detach時にclose, 0=呼び出し側が管理 */
} LoopSlot;
```

#### 2c. d88_seek_sector → loop_dev_seek_d88

現在 `v86_disk.c` に存在する `d88_seek_sector` と `loop_dev.c` 内部の
セクタ走査ロジックを統合し、`loop_dev_seek_d88` として公開する。

**統合元 (v86_disk.c L250-L330 相当):**
```c
/* 入力:
 *   trk_cyl/trk_head — 物理トラック選択 (SEEK 位置)
 *   id_c/id_h/id_r   — セクタID照合 (コマンド C/H/R)
 * 出力:
 *   ファイル内データオフセット (0=未発見)
 *   セクタデータ長, トラック内SPT */
```

#### 2d. loop_dev_read_chs 実装

```c
int loop_dev_read_chs(int slot, u8 cyl, u8 head, u8 sect, void *buf)
{
    LoopSlot *s = &slots[slot];
    if (!s->in_use) return -1;

    if (s->is_d88) {
        /* D88: 標準 CHS アクセス (物理=論理) */
        u32 data_len = 0;
        u16 trk_spt = 0;
        u32 off = loop_dev_seek_d88(slot,
            cyl, head, cyl, head, sect,
            &data_len, &trk_spt);
        if (off == 0) return -1;
        vfs_seek(s->fd, off, 0);
        return (vfs_read_fd(s->fd, buf, data_len) > 0) ? 0 : -1;
    } else {
        /* FDI/RAW: オフセット計算 */
        u32 offset = s->data_offset
            + (((u32)cyl * s->heads + head) * s->spt + (sect - 1))
            * s->bps;
        if (offset + s->bps > s->data_offset + s->data_size)
            return -1;
        vfs_seek(s->fd, offset, 0);
        return (vfs_read_fd(s->fd, buf, s->bps) > 0) ? 0 : -1;
    }
}
```

#### 2e. 既存 blk_read のラッパー化

```c
/* 既存 Device API (LBA) は CHS コアのラッパーに変更 */
static int d88_blk_read(Device *self, int lba, int count, void *buf)
{
    LoopSlot *s = (LoopSlot *)self->priv;
    int slot = (int)(s - slots);
    int i;
    u8 *p = (u8 *)buf;

    for (i = 0; i < count; i++) {
        int cur = lba + i;
        u8 sect = (u8)((cur % s->spt) + 1);
        int tmp = cur / s->spt;
        u8 head = (u8)(tmp % s->heads);
        u8 cyl  = (u8)(tmp / s->heads);
        if (loop_dev_read_chs(slot, cyl, head, sect, p) != 0)
            return -1;
        p += s->bps;
    }
    return 0;
}
```

---

### 3. v86_disk.c — 重複コード削除

#### 削除対象 (行番号は現在のファイル基準)

| 行範囲 | 内容 | 代替 |
|---|---|---|
| L47-57 | `d88_le16/le32` ヘルパー | loop_dev.c に既存 |
| L59-64 | `D88_MEDIA_*` 定数 | loop_dev.c に既存 |
| L43-44 | `D88_MAX_TRACKS`, `D88_SECT_HDR_SIZE` | loop_dev.c に既存 |
| L45 | `fdd_d88_track_table[164]` | LoopSlot.track_offset |
| L66-73 | `d88_cyls/heads/spt/sect_size`, `d88_dyn_geom` | loop_dev_get_geometry |
| L138-249 | `v86_disk_set_d88()` ヘッダ解析 | loop_dev_attach_fd |
| L250-330 | `d88_seek_sector()` (推定) | loop_dev_seek_d88 |

#### 追加コード

```c
#include "loop_dev.h"

/* v86 が使用中の loop_dev スロット (-1 = 未使用) */
static int v86_loop_slot = -1;

void v86_disk_set_d88(int fd, u32 file_size, fdc_media_t media)
{
    int slot;
    u8 cyls, heads, spt;
    u16 bps;
    u32 total;

    (void)media;
    (void)file_size;

    /* 空きスロットを探してアタッチ */
    for (slot = 0; slot < 4; slot++) {
        if (loop_dev_attach_fd(fd, slot) == 0) {
            v86_loop_slot = slot;
            break;
        }
    }
    if (v86_loop_slot < 0) {
        kprintf(0x0C, "[V86 D88] no free loop slot\n");
        return;
    }

    /* ジオメトリを取得して fdd_geom に反映 */
    loop_dev_get_geometry(slot, &cyls, &heads, &spt, &bps, &total);
    d88_dyn_geom.cyls  = cyls;
    d88_dyn_geom.heads = heads;
    d88_dyn_geom.spt   = spt;
    d88_dyn_geom.bps   = bps;
    /* sec_n, gap3, daua_high は loop_dev から別途取得するか算出 */
    fdd_geom = &d88_dyn_geom;
    fdd_is_d88 = 1;
    fdd_fd = fd; /* vfs_seek/read 用に残す */
}
```

#### READ DATA ループの変更

```c
/* 変更前 (v86_disk.c L682-L685) */
u32 data_off = d88_seek_sector(
    cur_trk_cyl, cur_trk_head,
    cur_id_c, cur_id_h, cur_sect_r,
    &sec_data_len, &trk_spt);

/* 変更後 */
u32 data_off = loop_dev_seek_d88(v86_loop_slot,
    cur_trk_cyl, cur_trk_head,
    cur_id_c, cur_id_h, cur_sect_r,
    &sec_data_len, &trk_spt);
```

WRITE DATA ループも同様に変更。

#### v86_disk_clear の変更

```c
void v86_disk_clear(void)
{
    if (v86_loop_slot >= 0) {
        loop_dev_detach(v86_loop_slot);
        v86_loop_slot = -1;
    }
    fdd_fd = -1;
    fdd_is_d88 = 0;
    fdd_use_physical = 0;
}
```

---

### 4. v86_session.c — フォーマット判定統合

#### 削除対象

`v86_boot_freedos` (L521-L693) と `v86_boot_native` (L771-L888) に
ほぼ同一のフォーマット判定コードが存在する。これを共通ヘルパーに統合。

#### 追加: 共通ヘルパー関数

```c
#include "loop_dev.h"

/* v86 イメージ設定 (D88/FDI/RAW 自動判別)
 * 戻り値: 0=成功 (v86_disk 設定済み), -1=失敗 */
static int v86_setup_disk_image(int fd, u32 file_size)
{
    int slot;

    /* loop_dev でアタッチ試行 (フォーマット自動判別) */
    for (slot = 0; slot < 4; slot++) {
        u32 total;
        int bps_val;
        if (loop_dev_status(slot, &total, &bps_val))
            continue; /* 使用中 */
        if (loop_dev_attach_fd(fd, slot) == 0) {
            /* D88/FDI/RAW が自動判別され、ジオメトリも設定済み */
            u8 cyls, heads, spt;
            u16 bps;
            u32 total_lba;
            int is_d88;

            loop_dev_get_geometry(slot, &cyls, &heads, &spt,
                                 &bps, &total_lba);
            /* loop_dev の内部状態から D88 か判定 */
            /* → loop_dev_is_d88(slot) API を追加するか、
             *    v86_disk_set_d88/set_file を slot 番号で呼ぶ */
            v86_disk_use_loop(slot);  /* 新 API */
            return 0;
        }
    }

    /* フォールバック: RAW 2HD として設定 */
    v86_disk_set_file(fd, 0, file_size, FDC_MEDIA_2HD_1232);
    return 0;
}
```

#### 呼び出し側変更

```c
/* v86_boot_freedos (変更後) */
int v86_boot_freedos(const char *path, const char *cmdline)
{
    ...
    fd = vfs_open(path, 0);
    ...
    v86_setup_disk_image(fd, file_size);

    /* IPL を CHS で読み出し (D88/FDI/RAW 共通) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    if (v86_loop_slot >= 0) {
        loop_dev_read_chs(v86_loop_slot, 0, 0, 1, ipl_dst);
    } else {
        vfs_seek(fd, current_session.img_offset, 0);
        vfs_read_fd(fd, ipl_dst, IPL_SIZE);
    }
    ...
}
```

---

## テスト計画

### 回帰テスト (必須)

| # | テスト | 期待結果 |
|---|---|---|
| 1 | `losetup /Ys.D88 0` + `dd lo0 lba=0 count=32` | LBA=16 でエラー停止 |
| 2 | `dd lo0 lba=0 count=2464 file=/host/ys.bin noerr` | 630,784B 完了 + エラー報告 |
| 3 | `losetup /dos5_1.fdi 0` + `dd lo0 count=1232 file=/host/fdi.bin` | ground truth 完全一致 |
| 4 | `exec Ys.D88` (v86 ネイティブブート) | ブート + FM 音楽再生 |
| 5 | `exec /dos5_1.fdi` (v86 FreeDOS ブート) | 正常起動 |

### 新機能テスト

| # | テスト | 期待結果 |
|---|---|---|
| 6 | `loop_dev_attach_fd` で同一 fd を2スロットにアタッチ | 2番目はエラー or 成功 (仕様決定要) |
| 7 | V86 + losetup 同時使用 | 異なるスロットで共存 |

## コミット戦略

```
feat: loop_dev: add CHS core API (read_chs, seek_d88, attach_fd)
refactor: v86_disk: replace d88_seek_sector with loop_dev_seek_d88
refactor: v86_session: unify format detection via loop_dev_attach_fd
refactor: loop_dev: convert blk_read to CHS-based wrapper
```
