# TASK_EXT2_EMPTY_NAME — NP21/W の NHD のルートに「名前の無いディレクトリ項目」があり、Linux の ext2 が読めない

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23 夜) / 状態: **調査中 (未着手)**。FS (カーネル層) の欠陥なので POLICY_DEV §1 に沿って優先。
> **明日の実機の CD インストール (TASK_HDD_INSTALL) と同じ経路 (`cdinst`) で作られた NHD で起きている**。

## 事実 (PM、読み取り専用で確認)

- `make nhd-pull` した NP21/W の NHD (`os32.nhd`、最終更新 2026-09-23 17:06) を Linux で ro マウントすると、ルートの各エントリの stat が I/O エラー → `make deploy-kernel` が前提検査で停止 (NHD には書いていない)。
- `e2fsck -n -f`: `Directory inode 2, block #0, offset 132: directory corrupted`。
- ルートのブロック 260 の offset 0x84: **inode 23、rec_len 8、name_len 0、type 2 (ディレクトリ)** — 名前の無い項目。続く 0x8C に inode 43 `db` (rec_len 884)。inode 23 は空のディレクトリ (`.` と `..` だけ)。
- 作成順 (inode 番号) は cdinst の mkdir の並び (`userland/system/cdinst.c:505〜520`: sys 11, boot 12, bin 13, sbin 14, usr 15, usr/bin 16, usr/man 17, etc 18, data 19, home 20, home/user 21, tmp 22) と一致し、**その直後に inode 23 (名前なし)**、後で `db` 43。日時はすべて `30-Mar-2025 15:43` (OS32 のフォーマッタの固定値)。= **この NHD は NP21/W 上の cdinst (CD インストール) で作られ**、cdinst の mkdir の後の段 (パッケージ展開) で**空の最終要素 (末尾が `/` のパスなど) の mkdir** が名前の無いディレクトリを作ったと推定 (未確認)。
- OS32 自身の ext2 はこの項目を読み飛ばすので NP21/W 上では気づかれなかった。Linux (e2fsck) は name_len = 0 を壊れと見なす。
- 2026-09-14 のバックアップ (`build/nhd/os32.nhd.bak-n1-20260914-101620`) は別の配置で、e2fsck は正常。

## 調べること

1. ext2 の mkdir / 項目追加が**空の名前 (長さ 0) を断るか** (`fs/ext2_dir.c`、VFS のパス分解で末尾 `/` や `//` がどう渡るか)。断らないならカーネルの欠陥。
2. cdinst / mkpkg のパッケージ展開で、ディレクトリ項目のパスが `/` で終わるものが渡るか (`tools/mkpkg.py` の出力、`rt/pkg.h`)。
3. 修正後、cdinst → e2fsck -n がきれいに通ることを受入に (ホストで NHD を作って検査できる)。
