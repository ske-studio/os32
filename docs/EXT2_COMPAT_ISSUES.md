# ext2 互換性問題メモ

## 発覚日: 2026-04-13

## 症状

NHDイメージ内のext2パーティションを Linux (`mount -t ext2 -o loop,offset=...`) でマウントしようとすると
`Structure needs cleaning` エラーが発生する。

`e2fsck -fy` で修復を試みると、**全ブロックグループ (1〜24) のblock bitmap / inode bitmap が
他のfsブロックと衝突 (conflicts with some other fs block)** しており、
ブロックグループ1,2についてはビットマップの再配置すら不可能で `aborted` となる。

```
Group 1's block bitmap at 8193 conflicts with some other fs block.
Group 1's inode bitmap at 8194 conflicts with some other fs block.
...
Error allocating 1 contiguous block(s) in block group 1 for block bitmap:
  Could not allocate block in ext2 filesystem
e2fsck: aborted
```

## 原因の可能性

### 1. OS32 カーネル側 (`fs/ext2_fmt.c` / install コマンド)

OS32の `ext2_fmt.c` が NHD の ext2 パーティションを初期化する際、
各ブロックグループのメタデータ (block bitmap, inode bitmap, inode table) の配置が
**標準 ext2 仕様と一致していない** 可能性がある。

具体的には:
- 各グループでビットマップがグループの先頭ブロック (N*8192+1, N*8192+2) に固定配置されているが、
  ブロックグループ0のスーパーブロック/GDTバックアップとの重複回避が正しく行われていない可能性
- バックアップスーパーブロックの配置が仕様通りでない (e2fsckが `Bad magic number in super-block while using the backup blocks` と報告)

### 2. nhd_deploy.py (debugfs ベースデプロイ)

`nhd_deploy.py` の `run_debugfs()` は以下の手順でファイルを書き込む:
1. `dd` でext2パーティションを一時RAWファイルに抽出
2. `debugfs -w` で一時ファイルに書き込み
3. NHDヘッダ+ブート領域 + 変更済みRAW を結合して書き戻し

この手順で以下の問題が起きうる:
- 抽出時に `dd` の `count` が指定されていないため、NHDファイル末尾まで読み込んでしまい、
  ext2パーティションの想定サイズを超えたゴミデータが付加される可能性
- 書き戻し時にファイルサイズが変わるとオフセットがずれる可能性

## 暫定対策

- `mkfs.ext2 -b 1024 -I 128 -L OS32_HDD` で Linux 標準ツールにてフォーマットし直す
- フォーマット後は Linux の `mount` で正常にマウント可能になることを確認
- OS32カーネル側でもマウントできるか、NP21/W上で検証する

## TODO

- [ ] `ext2_fmt.c` のブロックグループディスクリプタ配置ロジックをレビュー
- [ ] `nhd_deploy.py` の `dd` コマンドに `count` パラメータを追加
- [ ] Linux `mkfs.ext2` で作成したイメージがOS32カーネルで読めるか検証
- [ ] 問題が解消したら `install` コマンドのフォーマットロジックも修正
