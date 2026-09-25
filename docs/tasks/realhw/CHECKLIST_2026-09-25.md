# 実機 (PC-9821Ra266) の回 — 2026-09-25 の手順: HDD インストール

> 発行: PM (Claude Code `claude-opus-5-5`)。使う成果物: **CI の artifact `os32-feat-gui-e146022`** (KAPI **v65**、HDD インストール段 1+2、VK32 CRC、FD 高速化、ビープ修正)。
> ノートの取り方: `cd ~/os32 && git pull && tools/ci_fetch.sh --sha e146022` → `./os32-ci/os32-feat-gui-e146022/`。
> 前回の手順: [CHECKLIST_2026-09-24.md](CHECKLIST_2026-09-24.md)。票: [TASK_HDD_INSTALL.md](TASK_HDD_INSTALL.md)、[TASK_SERIAL_HOSTFS.md](TASK_SERIAL_HOSTFS.md) (部品 A)。

## 書き込む媒体

- **FD は生イメージ** `os32_boot.img` (2HD 1232KB) か `os32_boot144.img` (1.44MB)。`.d88` はエミュレータ用。
- **CD** は `os32_install.iso` を CD-R に焼く (または手元の CD エミュレーション)。

## 手順 (この順)

| # | 誰が | やること | 見るもの / 記録 |
|---|---|---|---|
| 1 | ユーザー | FD で起動 | 起動の時間、フォント読み込みの後の `[fdc] font:` 3 行 (`tmo=0` か)、`[boot] Build … Commit: e146022`、`[boot] Image CRC: … src=fd` |
| 2 | ノート | シリアルで `sync` → `cmd ver` | `API: v65`、`Commit: e146022`。**このときビープが鳴らないか** (ユーザーの耳) |
| 3 | ユーザー | CD を入れ、本体キーボードかシリアルで `cdinst` → `2` (Normal) | 確認画面の前に出る行 (下の表) を**写真に**。ここではまだ何も書いていない |
| 4 | ユーザー | 確認画面の内容を見て `y` | 下の「止めるべき表示」が出ていたら `N` |
| 5 | — | 展開 (数分) | 最後が `=== Installation Complete ===` か。`INCOMPLETE` なら写真 |
| 6 | ユーザー | FD と CD を抜いて再起動 (HDD 起動) | `[boot] Image CRC: … src=hdd`、`[selftest] N/N passed` |
| 7 | ノート | シリアルで `cmd ver`、`cmd "ls /sys/lib"`、`cmd uptime` | `Image CRC … HDD loader`、`libos32gui.shlib` |

### 手順 3 で出る行 (期待値)

- `hd0 BIOS (AH=84h): valid=1 C/H/S=16382/16/63 len=512`
- `hd0 ATA: present=1 total=16514063 sectors …`
- `Target: hd0 …, empty disk: create the OS32 area` (空のディスク) — 実機の 8GB に**他の OS の区画があれば**「This disk is not a target for the OS32 installer」と出る。
- **`not a target` と出たら ERASE** (wt/cdinst-wipe 以降の cdinst / install): 続けて `Current contents of hd0:` の要約 (LBA 0 の 55AA、各区画の mid・sid・名前・開始と終了のシリンダ) が出るので**写真に**。消してよければ `Type ERASE:` に `ERASE` (大文字) + Enter → `LBA 0 and 1 of hd0 erased and verified (all zero)` → 確認画面は `empty disk` → 改めて `y`。それ以外の入力は何も書かずに断る。`y/N` で `N` にすると区画表は消えたまま止まる (`INCOMPLETE`、次の実行は空のディスクとして入れられる)。
- `OS32 area: LBA 2016..526175 (256 MB), cyl 2..521 at 16 heads x 63 sectors`

### 止めるべき表示 (手順 4 で `N`)

- 幾何が 16/63 でない、`valid=0`、LBA の範囲が上と大きく違う。
- `empty disk` でも `re-create` でもない (他の区画・壊れた表)。

## 起動しない・止まったとき

| 表示 | 意味 | 次 |
|---|---|---|
| `VK32: …` (ローダ) | カーネルイメージの検査で止まった (CRC・長さ・範囲) | 写真。FD を書き直す |
| `[EXT2] hd0: partition table is in the pre-v64 OS32 layout` | 旧配置 (今回は出ないはず) | 写真 |
| HDD 起動で何も出ない | IPL / ローダ | FD で起動して `hdprep` はしない。写真と `ver` |

## やらないこと

- `hdprep` (空のディスク専用の別経路。今回は cdinst を使う)。
- HostDrv / hsync の実機での利用 (SerialFS は実装中)。

## 持ち帰るもの

写真 (手順 1・3・5・6)、ノートのシリアル出力、ビープの有無。
