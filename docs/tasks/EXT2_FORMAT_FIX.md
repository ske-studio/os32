# カーネル ext2 ドライバのマルチグループ対応

## 概要
カーネルの ext2 ドライバは **単一ブロックグループ前提** で実装されているため、
Linux 標準ツール (`mkfs.ext2`, `debugfs`, `mount`) で作成した複数グループの ext2 を正しく読み書きできない。ホスト側からNHDイメージにファイルを直接デプロイするためにマルチグループ対応が必要。

## 問題の2面性

### 問題A: `ext2_format()` の出力が非標準
カーネルの `ext2_format()` が生成するスーパーブロックの `s_blocks_per_group` が ext2 仕様の上限を超えている。

| フィールド | OS32出力値 | ext2仕様上限 |
|:---|:---|:---|
| s_blocks_per_group | 204,612 | **8,192** (1KBブロック時) |
| s_inodes_per_group | 51,153 | ビットマップ制約あり |

**原因**: 全ブロックを1グループに押し込んでいる。1KBブロックではブロックビットマップが1ブロック(1024バイト=8192ビット)しかないため、1グループ最大8,192ブロック。

### 問題B: カーネル側がマルチグループを読めない
`ext2_super.c` が **グループディスクリプタを1つだけ** (`ext2_gd_info`) 読み込み、
`ext2_inode.c` が inode 番号からグループを特定せず **常にグループ0のテーブルを参照** する。

```c
/* ext2_inode.c L15-16: グループ計算なし */
index = (ino - 1) % ext2_sb_info.inodes_per_group;
block_num = ext2_gd_info.inode_table + ...;  /* ← 常にグループ0 */
```

`mkfs.ext2` で作成したext2は25グループに分かれるため、inode 12 (グループ0) は読めても、大きい inode 番号のファイルは読めない。

## 影響範囲

| ファイル | 問題 |
|:---|:---|
| `fs/ext2_priv.h` | `Ext2GroupDesc ext2_gd_info` が1つだけ |
| `fs/ext2_super.c` | グループディスクリプタを1グループ分しか読まない |
| `fs/ext2_inode.c` | `ext2_read_inode()` / `ext2_write_inode()` にグループ計算なし |
| `fs/ext2_inode.c` | `ext2_alloc_block()` / `ext2_alloc_inode()` がグループ0のビットマップのみ使用 |
| `fs/ext2_fmt.c` | `ext2_format()` が非標準スーパーブロックを生成 |

## 方針

### 案A: カーネルをマルチグループ対応にする (推奨)
カーネル側を正しいext2仕様に準拠させる。ホスト側ツールとの完全互換性が得られる。

### 案B: ホスト側ツールをカーネル独自ext2に対応させる
`nhd_deploy.py` に独自ext2書き込み機能を実装する。移植性が悪い。

→ **案A** を採用する。

## タスクリスト

### Phase 1: グループディスクリプタテーブルの読み込み対応
- [ ] `ext2_priv.h`: `Ext2GroupDesc ext2_gd_info` → `Ext2GroupDesc ext2_gd_table[MAX_GROUPS]` に変更
  - 200MBディスク: ceil(204,612 / 8,192) = 25 グループ
  - `MAX_GROUPS = 32` で十分 (32 × 18B = 576B)
- [ ] `ext2_priv.h`: `u32 ext2_num_groups` グローバル変数を追加
- [ ] `ext2_super.c`: `ext2_mount()` でグループディスクリプタテーブル全体を読み込み
  - GDTはブロック2から連続配置 (1KBブロック時: 32B × 25 = 800B = 1ブロック)

### Phase 2: inode 読み書きのグループ対応
- [ ] `ext2_inode.c`: `ext2_read_inode()` にグループ番号計算を追加
  ```c
  u32 group = (ino - 1) / ext2_sb_info.inodes_per_group;
  u32 index = (ino - 1) % ext2_sb_info.inodes_per_group;
  block_num = ext2_gd_table[group].inode_table + ...;
  ```
- [ ] `ext2_inode.c`: `ext2_write_inode()` も同様に修正

### Phase 3: ブロック/inode アロケーションのグループ対応
- [ ] `ext2_alloc_block()`: 全グループのビットマップを走査
  - 現在: `ext2_gd_info.block_bitmap` (グループ0のみ)
  - 修正: `ext2_gd_table[g].block_bitmap` をグループ順に走査
- [ ] `ext2_free_block()`: `block_num` からグループを逆算
  ```c
  u32 group = (block_num - first_data_block) / blocks_per_group;
  u32 bit = (block_num - first_data_block) % blocks_per_group;
  ```
- [ ] `ext2_alloc_inode()` / `ext2_free_inode()` も同様

### Phase 4: ext2_format() の標準準拠
- [ ] `ext2_fmt.c`: `s_blocks_per_group = 8192` に設定
- [ ] 複数グループのGDT、ブロックビットマップ、inodeビットマップ、inodeテーブルを初期化
- [ ] スーパーブロックのバックアップコピーを各グループに書き込み (オプション)

### Phase 5: スーパーブロック/GD書き戻しの修正
- [ ] `ext2_write_super_raw()`: 変更なし (スーパーブロックは1つ)
- [ ] `ext2_write_gd_raw()`: 全グループのGDを書き戻し
- [ ] `ext2_sync()`: 全グループの dirty GD を書き戻し

### Phase 6: 検証
- [ ] `mkfs.ext2` で作成したext2パーティションを OS32 でマウントし、ls / cat / exec が正常動作
- [ ] OS32 の `ext2_format()` で作成したext2を Linux の `debugfs` / `mount` で認識
- [ ] `install` → HDD起動 → シェル動作が正常
- [ ] `nhd_deploy.py` (修正済み) でファイルデプロイ→OS32で読み取り確認

## 既存コードの参照箇所一覧

### グループ0ハードコードの全箇所

| ファイル | 行 | 問題 |
|:---|:---|:---|
| `ext2_priv.h:12` | `Ext2GroupDesc ext2_gd_info` | 単一グループ |
| `ext2_super.c:224-233` | GD読み込み | グループ0のみ |
| `ext2_super.c:121-129` | `ext2_write_gd_raw()` | グループ0のみ |
| `ext2_inode.c:15-16` | `ext2_read_inode()` | グループ計算なし |
| `ext2_inode.c:56-57` | `ext2_write_inode()` | グループ計算なし |
| `ext2_inode.c:100` | `ext2_alloc_block()` | グループ0ビットマップ |
| `ext2_inode.c:131` | `ext2_free_block()` | グループ計算なし |
| `ext2_inode.c:147` | `ext2_alloc_inode()` | グループ0ビットマップ |
| `ext2_inode.c:178` | `ext2_free_inode()` | グループ計算なし |

## 暫定対処 (ext2修正前)

```bash
# NP21/Wでinstall実行後、ホスト側でext2パーティションを再フォーマット
dd if=os32.nhd of=/tmp/ext2_part.img bs=512 skip=273
echo y | mkfs.ext2 -b 1024 -L OS32_HDD /tmp/ext2_part.img
# debugfsで全ファイルを書き込み (install相当)
for f in programs/*.bin; do
  name=$(basename "$f" | tr 'A-Z' 'a-z')
  debugfs -w -R "write $f $name" /tmp/ext2_part.img
done
debugfs -w -R "write assets/fep.dic fep.dic" /tmp/ext2_part.img
# NHDに書き戻し
dd if=/tmp/ext2_part.img of=os32.nhd bs=512 seek=273 conv=notrunc
```

**注意**: OS32カーネルはマルチグループext2を読めないため、この暫定手段はマルチグループ対応 (Phase 1-2) が完了するまで使用不可。

## 参考
- `programs/install.c` — HDDインストーラー
- `docs/NHD_FORMAT.md` — NHD r0ファイル構造仕様
- `tools/nhd_deploy.py` — NHDデプロイツール
