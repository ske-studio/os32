# カーネル ext2 ドライバのマルチグループ対応

## 概要
カーネルの ext2 ドライバは **単一ブロックグループ前提** で実装されていたため、
Linux 標準ツール (`mkfs.ext2`, `debugfs`, `mount`) で作成した複数グループの ext2 を正しく読み書きできなかった。

## ステータス: 完了

### Phase 1〜3, 5: マルチグループ対応 → ✅ 完了
- `ext2_priv.h`: `ext2_gd_table[MAX_GROUPS]` + `ext2_num_groups`
- `ext2_super.c`: `ext2_mount()` で全GDT読み込み対応
- `ext2_inode.c`: `ext2_read_inode()` / `ext2_write_inode()` にグループ計算追加
- `ext2_inode.c`: `ext2_alloc_block()` / `ext2_free_block()` / `ext2_alloc_inode()` / `ext2_free_inode()` がマルチグループ走査
- `ext2_super.c`: `ext2_write_gd_raw()` が全グループのGDを書き戻し

### Phase 4: ext2_format() の SPARSE_SUPER 対応 → ✅ 完了
- `is_sparse_group()` 判定関数追加 (0, 1, 3^n, 5^n, 7^n)
- `s_feature_ro_compat = SPARSE_SUPER (0x0001)` 設定
- スパースグループ: SB+GDT領域をスキップしてメタデータ配置
- SBバックアップ + GDTコピーをスパースグループに書き込み
- `free_inodes_count` のoff-by-one修正 (inodes_count-11 → -10)

### Phase 6: 検証 → 一部未完了
- [x] `mkfs.ext2` で作成したext2パーティションを OS32 でマウントし、ls / cat / exec が正常動作
- [x] `install` → HDD起動 → シェル動作が正常
- [x] `nhd_deploy.py` でファイルデプロイ→OS32で読み取り確認
- [ ] OS32 `ext2_format()` で作成したext2を Linux の `e2fsck` / `mount` で認識 → **SPARSE_SUPER修正後の検証が必要**
