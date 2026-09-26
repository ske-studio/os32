# 実機 (PC-9821Ra266) の回 — 2026-09-26 発行の手順: 入れ直しと v2.1 の確認

> 発行: PM (Claude Code `claude-opus-5-5`)。使う成果物: **CI の artifact `os32-feat-gui-b8f76e0`** (KAPI **v66**)。手順 10 (`kbdstat -w`) は **5992fd5 以降 (KAPI v67)** の成果物が要る — その回の最新の CI 成果物を使い、ver の Commit を記録する。
> ノートの取り方: `cd ~/os32 && git pull && tools/ci_fetch.sh --sha b8f76e0` → `./os32-ci/os32-feat-gui-b8f76e0/`。
> 前回の手順: [CHECKLIST_2026-09-25.md](CHECKLIST_2026-09-25.md) (HDD インストール、合格)。v2.1 の条件は [ROADMAP](../../ROADMAP.md) §0。

## 前回からの変わりどころ (この回で見るもの)

| 変更 | 実機で見ること |
|---|---|
| **MINIMAL の組み替え** — フォントと一般コマンドを NORMAL へ、hsync を MINIMAL へ (FD にもフォントが無い) | FD 起動で **赤い `FONT..NG` が出るのは正常** (ユーザー決定)。起動は止まらずに進むこと |
| **CD 読みの高速化** (READ(10) を 16 セクタ、先読み) と **ATAPI の装置選び** (マスター/スレーブ、UA・準備中の再試行、失敗の診断行) | cdinst の所要時間 (前回は Normal で **約 10 分**)。`[atapi] READ(10) …` の行が出たら写真 |
| **起動ログ** `/var/log/boot.log` (前回分は `boot.log.1`) | セルフテストの数を画面ではなくログで読む |
| **ERASE** (他 OS の区画・壊れた表を消して入れる) | 今の HDD は OS32 の区画なので**出てこない**のが正しい (下の「やらないこと」) |
| hsync は FD 起動では断る (`dest_on_fd`) | 手順 4 で 1 回だけ |

## 書き込む媒体

- **FD**: `os32_boot.img` (2HD 1232KB の生イメージ)。1.44MB を使うなら `os32_boot144.img`。
- **CD**: `os32_install.iso` を CD-R に焼く。

## 手順 (この順)

| # | 誰が | やること | 見るもの / 記録 |
|---|---|---|---|
| 1 | ユーザー | FD で起動 | `[boot] … Commit: b8f76e0`、`Image CRC … src=fd`、**赤い `FONT..NG` の後もプロンプトまで進む** |
| 2 | ノート | シリアルで `sync` → `cmd ver` | `API: v66`、`Commit: b8f76e0`。**このときビープが鳴らないか** (耳) |
| 3 | ユーザー | CD を入れて `cdinst` → `2` (Normal) | 今の HDD は OS32 なので `re-create` の確認画面。`y`。**開始から `=== Installation Complete ===` までの時間を計る** |
| 4 | ノート | (手順 3 の前でも後でもよい、FD 起動中に) `cmd hsync -n` | `reason=dest_on_fd` で断る (何も書かない)。1 回だけ |
| 5 | ユーザー | FD と CD を抜いて再起動 (HDD 起動) | `Image CRC … src=hdd` |
| 6 | ノート | `cmd "cat /var/log/boot.log"` | `# OS32 boot log … Commit b8f76e0`、`[selftest] N/N passed`、`[atapi]` の行の有無、`[pegc] hsync=` の行 |
| 7 | ノート | `cmd "ls -l /cd0"` (CD を入れたまま再起動した場合) か、CD を入れて `cmd "mount /cd0 cd0 iso9660"` → `cmd "ls -l /cd0"` | 5 本のパッケージが見える |
| 8 | ユーザー | `gfxmode pegc` → リセット → GUI 起動 → ずれた画面で**液晶の自動調整ボタン** | 直るか・変わらないか。液晶の OSD の**水平/垂直周波数と解像度の写真** ([TASK_PEGC480_REALHW](TASK_PEGC480_REALHW.md) 段 0) |
| 9 | ユーザー | `gfxmode pc98` に戻してリセット | CUI が正しく出る |
| 10 | ユーザー + ノート | 本体で `kbdstat -w` → カナを押し込む (3 秒保持) → 離す → もう一度押して解除 → CAPS で同じ → ESC | 画面の行を写真 ([TASK_KBD_NAV](../gui/TASK_KBD_NAV.md) §3)。**解除のときに break が来るか**、押している間に make が繰り返すか。NP21/W は「押下で make・解除で break」だった (§3-1) |

## v2.1 の判定 (手順 1〜7)

- 1・5 で起動が止まらない、2 でビープが鳴らない、3 が `Installation Complete`、6 でセルフテストが全部通る → **v2.1 を feat/gui → main に取り込んでタグを打つ** (PM)。
- 手順 8・9 (PEGC) は v3 の材料で、v2.1 の判定には入れない。

## 止まったとき

| 表示 | 意味 | 次 |
|---|---|---|
| `VK32: …` (ローダ) | カーネルイメージの検査で止まった | 写真。FD を書き直す |
| cdinst の `NOT FOUND` / `BOOT.PKG not found` | CD が読めない | `[atapi] READ(10) drv= … sense=` の行を写真 (ドライブの位置と理由が出る) |
| cdinst が極端に遅い | CD 読みの待ち | 経過時間と画面の写真。`[atapi]` の行 |
| `INCOMPLETE` | 展開の途中で失敗 | 写真。再起動はしない |

## やらないこと

- **ERASE の試験**: 他の OS の区画がある HDD が要る。今の HDD は OS32 の区画なので確認画面に出ない (出たら写真)。
  試すなら、ノートで壊れた表を書いた別の HDD を用意する回にする。

- インストール後の e2fsck (ユーザー決定でしない)。

## 持ち帰るもの

写真 (手順 1・3 の終わり・5・8 の OSD)、cdinst の所要時間、ノートのシリアル出力 (手順 2・4・6・7)、ビープの有無。
