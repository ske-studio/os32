# ext2 互換性問題メモ

## 発覚日: 2026-04-13
## 解決日: 2026-04-13

## 症状

NHDイメージ内のext2パーティションを Linux (`mount -t ext2 -o loop,offset=...`) でマウントしようとすると
`Structure needs cleaning` エラーが発生する。

`e2fsck -fy` で修復を試みると、**全ブロックグループ (1〜24) のblock bitmap / inode bitmap が
他のfsブロックと衝突 (conflicts with some other fs block)** と報告されていた。

## 根本原因

### 1. `ext2_format()` の SPARSE_SUPER 非対応

`s_feature_ro_compat = 0` (SPARSE_SUPER 未設定) のため、Linux は全グループに
SBバックアップが必要と判断するが、実際にはグループ先頭にビットマップを配置していたため衝突。

### 2. `s_feature_incompat` に FILETYPE 未設定

ディレクトリエントリの `file_type` フィールドを使用しているのに、スーパーブロックの
feature フラグに宣言していなかった。e2fsck が `file_type` バイトを `name_len` の
上位バイトとして誤解釈し、ディレクトリ破損と判定。

### 3. `install.c` の `sys_mkdir("/hd0")` 呼び出し

`/hd0` はマウントポイント(ext2のルートそのもの)であり、`sys_mkdir()` を呼ぶと
`name_len=0, rec_len=8` の不正なディレクトリエントリが生成されていた。

## 修正内容

### `fs/ext2_fmt.c`

1. **SPARSE_SUPER 対応**
   - `s_feature_ro_compat = SPARSE_SUPER (0x0001)` を設定
   - スパースグループ判定関数 `is_sparse_group()` 追加 (0, 1, 3^n, 5^n, 7^n)
   - スパースグループ: SB+GDT分をスキップしてメタデータ配置
   - SBバックアップ + GDTコピーをスパースグループに書き込み

2. **FILETYPE フラグ設定**
   - `s_feature_incompat = FILETYPE (0x0002)` を設定

3. **free_inodes_count 修正**
   - `inodes_count - 11` → `inodes_count - 10` (予約inode 1-10のみ)

### `programs/install.c`

4. **不正な sys_mkdir 削除**
   - `sys_mkdir("/hd0")` を削除 (マウントポイントはmkdir不要)

## 検証結果

50MB NHD (7グループ) でインストーラーを実行し、Linux側から検証:

```
$ e2fsck -fn /dev/loop0
Pass 1: Checking inodes, blocks, and sizes      ✅
Pass 2: Checking directory structure             ✅
Pass 3: Checking directory connectivity          ✅
Pass 4: Checking reference counts                ✅
Pass 5: Checking group summary information       ✅
OS32_HDD: 47/12768 files, 2205/51000 blocks
```

- [x] e2fsck エラーなし
- [x] SPARSE_SUPER レイアウト正常 (7グループ: スパース=0,1,3,5,7 / 非スパース=2,4,6)
- [x] FILETYPE フラグ正常
- [x] ディレクトリ構造正常
