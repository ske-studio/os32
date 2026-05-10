# Phase B: v86_disk の薄いブリッジ化

## 目的

`v86_disk.c` から独自のジオメトリ管理・ディスクイメージ管理を撤去し、
`loop_dev` の薄いブリッジ (フロントエンド) に変換する。

---

## B-1. 公開 API 再設計

### 対象ファイル

- `kernel/v86_disk.h`

### 削除する関数

| 関数 | 理由 |
|------|------|
| `v86_disk_set_file(int fd, u32 offset, u32 size, fdc_media_t)` | loop_dev 経由に一本化 |
| `v86_disk_set_d88(int fd, u32 file_size, fdc_media_t)` | loop_dev 経由に一本化 |
| `v86_disk_get_geom()` → `const struct fdc_geom *` | 新API に置換 |
| `v86_disk_get_fd()` | loop_dev 経由で取得 |
| `v86_disk_get_offset()` | loop_dev 内部で管理 |
| `v86_disk_is_physical()` | 新API に置換 |
| `v86_disk_get_phys_drv()` | 新API に置換 |

> **互換ラッパー**: Phase 9 (最終ステップ) まで、削除対象関数はインラインで
> 新 API を呼ぶ薄いラッパーとして残す。v86_fdc.c 改修 (B-4) 完了後に削除。

### 新規/変更する関数

```c
/* loop_dev スロットをアタッチ (FDD/HDD自動判別) */
void v86_disk_attach_loop(int slot);

/* 実FDDモード (据え置き) */
void v86_disk_set_physical(int drv, fdc_media_t media);

/* クリア */
void v86_disk_clear(void);

/* loop_dev スロット番号取得 (-1=未設定) */
int  v86_disk_get_loop_slot(void);

/* ジオメトリ取得 (loop_dev 経由) */
int  v86_disk_get_geometry(u16 *cyls, u8 *heads, u8 *spt,
                           u16 *bps, u8 *sec_n, u8 *daua_high);

/* 実FDDモード判定 */
int  v86_disk_is_loop(void);     /* 1=loop_dev 経由 */
int  v86_disk_is_phys(void);     /* 1=物理FDD */
int  v86_disk_get_phys_drv_num(void);  /* 物理ドライブ番号 */
```

> **`daua_high` の追加**: INT 1Bh の DA/UA バリデーション (v86_disk.c L278) と
> FORMAT TRACK (v86_fdc.c L350) で `daua_high` が必要。
> HDDモード (`0x80`) 追加時にも必須のため、`v86_disk_get_geometry` の引数に含める。
> `daua_high` の値は:
> - FDD 2HD: `0x90`
> - FDD 2DD: `0x70` (640KB) / `0x70` (720KB)
> - HDD: `0x80`
> - 実FDD: ジオメトリテーブルから取得

---

## B-2. 内部状態の単純化

### 対象ファイル

- `kernel/v86_disk.c`

### 新しい内部状態

```c
static int v86_loop_slot   = -1;     /* loop_dev スロット (-1=未設定) */
static int v86_phys_drv    = -1;     /* 物理FDDドライブ番号 (-1=未使用) */
static fdc_media_t v86_phys_media = FDC_MEDIA_2HD_1232;
static u8  fdc_treg        = 0;      /* SEEK トラックレジスタ (現状維持) */
```

### 撤去する変数

| 変数 | 代替 |
|------|------|
| `fdd_fd` | `loop_dev_get_fd(v86_loop_slot)` |
| `fdd_image_offset` | `loop_dev` 内部 (`data_offset`) |
| `fdd_image_size` | `loop_dev` 内部 (`file_size`) |
| `fdd_is_d88` | `loop_dev_get_format(v86_loop_slot) == LOOP_FMT_D88` |
| `d88_dyn_geom` | `loop_dev_get_geometry()` で動的取得 |
| `fdd_geom` (ポインタ) | `v86_disk_get_geometry()` で動的取得 |
| `fdd_use_physical` | `v86_phys_drv >= 0` |
| `fdd_phys_drv` | `v86_phys_drv` (名前変更のみ) |

---

## B-3. INT 1Bh ハンドラ簡素化

### 対象ファイル

- `kernel/v86_disk.c` — `v86_bios_int1b()`

### 3経路 → 2経路への縮約

現在:
```
v86_bios_int1b
  ├── physical → fdc_read_sector_geom()
  ├── D88      → loop_dev_seek_d88() + vfs_read_fd()
  └── RAW/FDI  → vfs_seek() + vfs_read_fd()
```

変更後:
```
v86_bios_int1b
  ├── physical → fdc_read_sector_geom() (据え置き)
  └── loop     → loop_dev_read_chs() / loop_dev_write_chs()
                  (D88 コピープロテクション対応は専用パスで残す)
```

### D88 コピープロテクション対応

Ys 等のゲームは SEEK 位置とコマンド CL 値を意図的に異ならせる。
`loop_dev_read_chs()` は物理=論理の CHS アクセスのみ対応するため、
D88 モードでは `loop_dev_seek_d88()` を直接呼ぶパスを残す:

```c
if (loop_dev_get_format(v86_loop_slot) == LOOP_FMT_D88) {
    /* D88 コピープロテクション対応パス */
    u32 data_off = loop_dev_seek_d88(
        v86_loop_slot,
        fdc_treg, fdc_hd,       /* トラック選択: SEEK物理位置 */
        cylinder, head_dh, sector_r,  /* セクタID照合: コマンドCHS */
        &sec_data_len, &trk_spt);
    /* ... vfs_read_fd ... */
} else {
    /* FDI/HDI/RAW: 標準 CHS アクセス */
    loop_dev_read_chs(v86_loop_slot, cylinder, head_dh, sector_dl + 1, dst);
}
```

### HDD 対応 (INT 1Bh DA/UA)

`loop_dev_is_hdd(v86_loop_slot)` が真の場合:

- DA/UA 上位ニブル `0x80` 系を許容 (現在は `0x90` のみ)
- セクタ長コード (CH) 検証を緩和: 256B/512B/1024B を許容し、
  `loop_dev_get_sec_n(v86_loop_slot)` との一致を確認
- CHS は `v86_disk_get_geometry()` から取得
- **AH のファンクションコードは FDD と同一** (0x06=READ, 0x05=WRITE)。
  AH=0x86 は PC-98 SCSI BIOS であり、ここでは使用しない。

### デバッグコードの切り出し

`v86_disk.c` L578-651 の Ys FM音源デバッグ用スナップショットコード (83行) を
`v86_debug.c` の専用関数 `v86_debug_disk_read_snapshot()` に移動する。
INT 1Bh READ 成功後に `v86_debug_enabled` ガード付きで呼び出す。

---

## B-4. v86_fdc.c 改修

### 対象ファイル

- `kernel/v86_fdc.c`

### 旧 API 使用箇所一覧 (全12箇所)

| 行 | 旧 API | 新 API |
|----|--------|--------|
| 165 | `v86_disk_get_geom()` | `v86_disk_get_geometry(...)` |
| 213 | `v86_disk_is_physical()` | `v86_disk_is_phys()` |
| 215 | `v86_disk_get_phys_drv()` | `v86_disk_get_phys_drv_num()` |
| 223 | `v86_disk_get_phys_drv()` | `v86_disk_get_phys_drv_num()` |
| 233 | `v86_disk_get_fd()` | `loop_dev_get_fd(v86_disk_get_loop_slot())` |
| 234 | `v86_disk_get_offset()` | (撤去: loop_dev 内部で管理) |
| 276 | `v86_disk_get_geom()` | `v86_disk_get_geometry(...)` |
| 310 | `v86_disk_get_geom()` | `v86_disk_get_geometry(...)` |
| 389 | `v86_disk_is_physical()` | `v86_disk_is_phys()` |
| 390 | `v86_disk_get_phys_drv()` | `v86_disk_get_phys_drv_num()` |
| 399 | `v86_disk_get_fd()` | `loop_dev_get_fd(v86_disk_get_loop_slot())` |
| 400 | `v86_disk_get_offset()` | (撤去: loop_dev 内部で管理) |

### 変換パターン

#### `v86_disk_get_geom()` → `v86_disk_get_geometry()` (3箇所)

```c
/* 旧 */
const struct fdc_geom *g = v86_disk_get_geom();
if (sec_n != g->sec_n) { ... }

/* 新 */
u16 g_cyls; u8 g_heads, g_spt, g_sec_n, g_daua; u16 g_bps;
v86_disk_get_geometry(&g_cyls, &g_heads, &g_spt, &g_bps, &g_sec_n, &g_daua);
if (sec_n != g_sec_n) { ... }
```

#### ファイルモード WRITE/READ (2箇所ずつ)

```c
/* 旧 */
int fd      = v86_disk_get_fd();
u32 img_off = v86_disk_get_offset();
vfs_seek(fd, img_off + byte_offset, 0);

/* 新: loop_dev_write_chs / loop_dev_read_chs を使用 */
int slot = v86_disk_get_loop_slot();
loop_dev_write_chs(slot, id_c, id_h, id_r, fmt_sector);
```

#### FORMAT TRACK (fdc_execute_format, L304-416)

FORMAT TRACK も `loop_dev_write_chs` に統一:

```c
/* 旧: CHS → バイトオフセット → vfs_seek + vfs_write_fd */
vfs_seek(fd, img_off + byte_offset, 0);
vfs_write_fd(fd, fmt_sector, (u32)g->bps);

/* 新 */
loop_dev_write_chs(v86_disk_get_loop_slot(), id_c, id_h, id_r, fmt_sector);
```

---

## B-5. v86_boot_freedos D88 パス修正

### 対象ファイル

- `kernel/v86_session.c` — `v86_boot_freedos()`

### 問題

`v86_boot_freedos()` (L536-611) の D88 分岐は Phase 1 統合 **以前** の
レガシーコードが残っており、`v86_disk_set_file()` を2回呼ぶ不正な処理になっている:

```c
/* L599-607: 2回呼びのレガシーコード */
v86_disk_set_file(fd, current_session.img_offset,
                  (u32)77 * 2 * 8 * 1024, media);  /* 仮サイズ */
g = v86_disk_get_geom();
current_session.img_data_size = (u32)g->cyls * g->heads * g->spt * g->bps;
v86_disk_set_file(fd, current_session.img_offset,
                  current_session.img_data_size, media);  /* 正サイズ */
```

一方、`v86_boot_native()` (L814) は正しく `v86_disk_set_d88()` を使用している:

```c
v86_disk_set_d88(fd, file_size, media);
```

### 修正内容

`v86_boot_freedos()` の D88 分岐を `v86_boot_native()` と同一の
`v86_disk_set_d88()` 呼び出しに修正する。

```c
/* 修正後 (v86_boot_native と同一) */
v86_disk_set_d88(fd, file_size, media);
{
    u32 trk0_off = *(u32 *)(hdr + 0x20);
    if (trk0_off >= 0x2B0 && trk0_off < file_size) {
        current_session.img_offset = trk0_off + 16;
    } else {
        current_session.img_offset = 0x2C0;
    }
}
```

> **重要**: この修正は Phase C (session 統合) の **前** に行う。
> 共通関数にレガシーコードを持ち込まないため。

---

## B 全体の検証

| 項目 | 手順 | 期待結果 |
|------|------|----------|
| D88 回帰 | `vdos -native Ys.D88` | タイトル画面到達 |
| FDI 回帰 | `vdos game.fdi` | FreeDOS ブート |
| 実FDD回帰 | `vdos` (引数なし) | 実FDD ブート |
| WRITE | D88 ゲームでセーブ操作 | データ書き込み成功 |
| FORMAT | FreeDOS で `FORMAT A:` | フォーマット成功 |
| ビルド | `make all` | エラー 0 |
