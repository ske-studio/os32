# 実機 (PC-9821Ra266) の回 — 2026-09-26 発行の手順: 入れ直しと v2.1 の確認

> 発行: PM (Claude Code `claude-opus-5-5`)。使う成果物: **CI の artifact `os32-feat-gui-b8f76e0`** (KAPI **v66**)。手順 10・11 は **カナ・CAPS を方式 B (make で ON・break で OFF) にした成果物 — e2ab7b1 を取り込んだ feat/gui 以降** が要る (古い成果物だと「外してもカナのまま」が方式の判定と混ざる) — その回の最新の CI 成果物を使い、ver の Commit を記録する。
> ノートの取り方: `cd ~/os32 && git pull && tools/ci_fetch.sh --sha b8f76e0` → `./os32-ci/os32-feat-gui-b8f76e0/`。
> 前回の手順: [CHECKLIST_2026-09-25.md](CHECKLIST_2026-09-25.md) (HDD インストール、合格)。v2.1 の条件は [ROADMAP](../../ROADMAP.md) §0。

## 前回からの変わりどころ (この回で見るもの)

| 変更 | 実機で見ること |
|---|---|
| **MINIMAL の組み替え** — フォントと一般コマンドを NORMAL へ、hsync を MINIMAL へ (FD にもフォントが無い) | FD 起動で **赤い `FONT..NG` が出るのは正常** (ユーザー決定)。起動は止まらずに進むこと |
| **CD 読みの高速化** (READ(10) を 16 セクタ、先読み) と **ATAPI の装置選び** (マスター/スレーブ、UA・準備中の再試行、失敗の診断行) | cdinst の所要時間 (前回は Normal で **約 10 分**)。`[atapi] READ(10) …` の行が出たら写真 |
| **起動ログ** `/var/log/boot.log` (前回分は `boot.log.1`) | セルフテストの数を画面ではなくログで読む |
| **ERASE** (他 OS の区画・壊れた表を消して入れる) | 今の HDD は OS32 の区画なので**出てこない**のが正しい (下の「やらないこと」) |
| hsync は FD 起動では断る (`dest_on_fd`) | 手順 4 で 1 回だけ。FD 起動から HDD を更新する口 `--root /hd0` は下の「追加」節 |

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
| 7a | ノート | (**KAPI v68 = `v86 -g` を取り込んだ feat/gui 以降の成果物のときだけ**。CUI のまま、GUI は起動しない) `python3 tools/rshell_serial.py --port /dev/ttyUSB0 --timeout 120 cmd "v86 -g -t"` → 続けて `... cmd "v86 -g"`。出力を**全部**ファイルへ写す (`> v86g_ra266.txt`) | NP21/W では `-g -t` と `-g` を 2026-09-26 に通した (票 §3 段 1 の記録)。`-g -t` は `R result   : OK` (`failbits 0000`、時間の見切りの自己試験で 0.3 秒ほど止まる)。`-g` は画面が一瞬 640x480 に切り替わって戻る。**ROM の 1 呼び出しは 3 秒で見切る** — 3 秒以上止まったら `R exit : timeout` で戻るはず。戻らないときは CTRL+GRPH+DEL (ROM が自分で CLI して回っているとそれも効かない — リセットして画面の写真)。`R 30h 480` が `AH≠05` なら ROM は何もしておらず戻しは呼ばない (`R 30h back` は `-`)。`R 31h` の AX/BX と `layout=`、`R 30h 480` / `R 30h back` の `-> AX=05..`、`O` の行 (ROM が出した OUT の列) と `I` の行。`R result` が OK 以外でもそのまま写す (決められなかった値・打ち切りの場所が材料)。**戻った後に CUI がずれていたら写真**して リセット ([TASK_PEGC480_REALHW](TASK_PEGC480_REALHW.md) §3 段 1) |
| 8 | ユーザー | `gfxmode pegc` → リセット → GUI 起動 → ずれた画面で**液晶の自動調整ボタン** | 直るか・変わらないか。液晶の OSD の**水平/垂直周波数と解像度の写真** ([TASK_PEGC480_REALHW](TASK_PEGC480_REALHW.md) 段 0) |
| 9 | ユーザー | `gfxmode pc98` に戻してリセット | CUI が正しく出る |
| 10 | ユーザー + ノート | 本体で `kbdstat -w` → カナを押し込む (3 秒保持) → 離す → もう一度押して解除 → CAPS で同じ → ESC | 画面の行を写真 ([TASK_KBD_NAV](../gui/TASK_KBD_NAV.md) §3)。**解除のときに break が来るか**、押している間に make が繰り返すか。NP21/W は「押下で make・解除で break」だった (§3-1) |
| 11 | ユーザー + ノート | カナを**ロックしたまま**再起動 → 起動行の `[kbd] … lock=` と、`kbdstat -w` の 1 行目の `mods=` | `lock=04` (カナ) なら BIOS が電源投入・再起動時のロックを 053Ah に載せている。`lock=00` なら載せていない (1 回ロックし直せば追いつく) |

## 追加: FD 起動 → SerialFS → `hsync --root /hd0` → HDD 起動 (更新の回し方)

HDD のカーネル (v65) は SerialFS を持たないので、HDD の更新は**新しいカーネルの FD で起動して** `/hd0` を宛先に回す。
**この節は `--root` を取り込んだ feat/gui 以降の成果物が要る** (b8f76e0 の hsync は `--root` を知らない = `unknown option` で止まる)。FD と `hostdrv.tar.gz` は同じ成果物から取る。
正典は [TASK_SERIAL_HOSTFS](TASK_SERIAL_HOSTFS.md) §5 (手順の理由・判断待ち)、オプションは `docs/manpages/hsync.1`「同期先の根 (--root)」。

ノートの準備 (`<SHA>` はその回の成果物):

```bash
tools/ci_fetch.sh --sha <SHA>
tar -xzf ./os32-ci/os32-feat-gui-<SHA>/hostdrv.tar.gz -C ./os32-ci/os32-feat-gui-<SHA>/
H=./os32-ci/os32-feat-gui-<SHA>/hostdrv
S="python3 tools/rshell_serial.py --port /dev/ttyUSB0 --fast 115200 --timeout 60 --serve-host $H"
```

| # | 誰が | やること | 見るもの / 記録 |
|---|---|---|---|
| R1 | ユーザー | 新しい FD で起動 (HDD は繋いだまま) | `Commit: <SHA>`。ノートで `cmd "ls /hd0/boot"` が見える |
| R2 | ノート | `$S cmd "sfs run hsync -n --root /hd0 --no-backup boot"` | `hsync: /host/boot -> /hd0/boot`、`PLAN /hd0/boot/vmkernel.lz4`、`sfs: exit=0`。`reason=root_…` / `dest_on_fd` なら止めて写す |
| R3 | ノート | `$S cmd "sfs run hsync --root /hd0 --no-backup boot"` | `UPDATE /hd0/boot/vmkernel.lz4`、`sfs: exit=0`。**所要時間を計る** (見積もり約 40 秒、実測ではない) |
| R4 | ノート | `$S cmd "sfs run hsync --root /hd0 sys"` | `sfs: exit=0` |
| R5 | ノート | `$S cmd "sfs run hsync --root /hd0"` | `PROTECTED /hd0/etc/settings.db` は正常、`sfs: exit=0`。**所要時間を計る** (初回の全体は約 10MB で 15 分程度の見積もり、実測ではない) |
| R6 | ユーザー | FD を抜いて再起動 (HDD 起動) | `Image CRC … src=hdd`、プロンプトまで進む |
| R7 | ノート | `python3 tools/rshell_serial.py --port /dev/ttyUSB0 cmd ver` | `Commit: <SHA>`、`API: v66` 以上 = HDD のカーネルが入れ替わった |

- `cmd` の行は**必ず引用符で括る** — 括らないと `--root` を rshell_serial.py が拾って止まる。
- R3 の `--no-backup` は必須 (FD 起動では「起動した版」が FD の版なので `vmkernel.old` の門が `not_booted_image` で断る)。HDD の旧カーネルは残らない。起動しなくなったら**この FD で起動**し、**前の成果物** (`tools/ci_fetch.sh --sha <前の SHA>` を展開して `--serve-host` に指す) で R3〜R5 をやり直す (同じ成果物を同期し直しても HDD 起動だけの障害は直らない)。`--no-backup` を付けない dry-run は `FAIL … reason=not_booted_image` と `--root` の案内で exit=1 になる — 門が効いている印で、壊れてはいない。
- R3〜R5 は 1 回の FD 起動の中で続けて打つ (途中でやめて HDD 起動すると、カーネルと `/sys` の版が食い違い得る)。
- 次の回からは HDD 起動のまま `sfs run hsync boot` → 再起動 → `sfs run hsync sys` / `sfs run hsync` (`--root` 無し、`.old` も作られる)。

## v2.1 の判定 (手順 1〜7)

- 1・5 で起動が止まらない、2 でビープが鳴らない、3 が `Installation Complete`、6 でセルフテストが全部通る → **v2.1 を feat/gui → main に取り込んでタグを打つ** (PM)。
- 手順 8・9 (PEGC) は v3 の材料で、v2.1 の判定には入れない。

**手順 10 で解除のときに break が来ず make だけだった場合 (方式 A のキーボード)**: 方式 B ではカナ・CAPS を外せない。CAPS が立ったままだと本体キーボードの入力が大文字になる (シリアルからの操作は影響なし)。PM が drivers/kbd.c の `kbd_lock_apply` を「make で反転・break を無視」に戻す (試験の変異 12 の形)。

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

写真 (手順 1・3 の終わり・5・8 の OSD)、cdinst の所要時間、ノートのシリアル出力 (手順 2・4・6・7・7a — 7a は `v86 -g` の全行)、ビープの有無。
