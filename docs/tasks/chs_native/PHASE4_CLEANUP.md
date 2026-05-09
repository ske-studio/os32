# Phase 4: クリーンアップ + 最終検証

## 目的

- 不要になった旧コードの削除
- 全デバイスの統合テスト
- ドキュメント更新

## 前提条件

- Phase 1-3 全て完了

---

## 削除対象ファイル・コード

### 1. [DELETE] d88_loop.c

旧 D88 ループバックドライバ。`loop_dev.c` に統合済みだがファイルが残存。

**確認事項:**
- `build/kernel.mk` の `C_KERNEL` から除外されているか
- 他ファイルからの `#include` がないか

### 2. disk.h / disk.c — 旧 LBA API 削除

```c
/* 削除候補 */
int disk_read_lba(int drv, int lba, int count, void *buf);
int disk_write_lba(int drv, int lba, int count, const void *buf);
void disk_lba_to_chs(int lba, int *cyl, int *head, int *sect);
int disk_chs_to_lba(int cyl, int head, int sect);
```

**条件**: `dev.c` の `dev_blk_read_lba` が全ての LBA アクセスを担うため、
`disk_read_lba` の呼び出し元がゼロになっていることを `grep` で確認。

### 3. ide.c — 旧 LBA API 削除

```c
/* 削除候補 */
int ide_read_sector(int drive, u32 lba, void *buf);
int ide_write_sector(int drive, u32 lba, const void *buf);
int ide_read_sectors(int drive, u32 lba, u32 count, void *buf);
int ide_write_sectors(int drive, u32 lba, u32 count, const void *buf);
static void ide_set_chs(int drive, u32 lba, u8 count);  /* LBA引数版 */
```

**条件**: `dev.c` の hd0-3 が `ide_read_sector_chs` を使用しており、
旧 `ide_read_sectors(lba)` の呼び出し元がゼロであること。

### 4. v86_disk.h — 旧宣言整理

不要になった宣言の削除:
- `v86_disk_set_d88` のシグネチャが変わった場合、旧版を削除
- D88 関連の外部公開が不要になったものを整理

---

## 最終検証チェックリスト

### ビルド検証

```bash
make clean
make all
```

全警告を確認し、未使用関数・変数の警告がないことを確認。

### 全デバイス統合テスト

| # | テスト | デバイス | 確認事項 |
|---|---|---|---|
| 1 | `ver` | — | Kernel/Shell タイムスタンプ最新 |
| 2 | `cat /etc/profile` | hd0 | IDE 読み出し |
| 3 | `echo test > /tmp/t.txt` + `cat /tmp/t.txt` | hd0 | IDE 書き込み |
| 4 | `losetup /dos5_1.fdi 0` + dd | lo0 | FDI CHS 読み出し |
| 5 | `losetup /Ys.D88 0` + dd noerr | lo0 | D88 CHS + エラー処理 |
| 6 | `exec Ys.D88` | v86+loop | D88 ネイティブブート |
| 7 | `exec /dos5_1.fdi` | v86 | FDI ブート |
| 8 | `mount cd0 /cdrom` + `ls /cdrom` | cd0 | ISO9660 (LBA) |
| 9 | `format fd0` (可能な場合) | fd0 | FDD CHS 書き込み |

### コード品質チェック

- [ ] `grep -r "blk_read\b" --include="*.c"` で旧 LBA 直接呼び出しが残っていないこと
- [ ] `grep -r "disk_read_lba\|disk_write_lba"` で呼び出し元ゼロ
- [ ] `grep -r "ide_read_sectors\|ide_write_sectors"` で呼び出し元ゼロ
- [ ] v86_disk.c に D88 パースコードが残っていないこと
- [ ] `sizeof(Device)` の変化を記録

---

## ドキュメント更新

### 更新対象

| ドキュメント | 更新内容 |
|---|---|
| `docs/DEVELOPMENT.md` | Device API の CHS/LBA 2系統について追記 |
| `docs/KAPI_SPEC.md` | `dev_blk_read` の内部動作変更について注記 |
| `GEMINI.md` | 必要に応じてドライバ構成の記述を更新 |

## コミット戦略

```
chore: remove deprecated d88_loop.c
refactor: disk.c: remove disk_read_lba (replaced by dev_blk_read_lba)
refactor: ide.c: remove LBA-based ide_read_sectors
docs: update DEVELOPMENT.md with CHS/LBA dual API description
```
