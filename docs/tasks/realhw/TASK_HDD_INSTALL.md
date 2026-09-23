# TASK_HDD_INSTALL — 実機 Ra266 の 8GB IDE へ置き場を作り、CD からインストールして HDD 起動する

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23) / 状態: **v2 — ラリー 1 (Fable / Codex とも Request changes) を反映、ラリー 2 待ち**。実装はキーボード修正 (wt/kbd-rty) の着地後。
> ユーザー指示 (2026-09-23): 「キーボードが直ってから HDD インストールへの移行を早める。FDD のみで一時的に置ける場所が無いのは不便。CD イメージからインストールする」。
> 正典の親: [PLAN.md](PLAN.md) §2 (8GB は未知の領域、先頭に小さく切る) / §3 (FD 起動 + CD から入れる)。

## 0. 事実 (PM がコードと実機で確認、2026-09-23)

| # | 事実 | 根拠 |
|---|---|---|
| F1 | 実機の HDD は IDENTIFY で **C=16382 H=16 S=63、16,514,063 セクタ** (約 8.4GB)。`[ide] drive0 identify=0`、既存の FAT は無い (`[fatfs] mount failed pdrv=1 err=13`) | 起動画面の写真 (CI ビルド 527255b) |
| F2 | 2 つのインストーラ (`userland/system/cdinst.c`、FD 用 `install.c`) は **8 ヘッド × 17 セクタの固定幾何**で区画表 (LBA 1) と IPL (`ipl[8]/[9]`) を書く。ブート予約は 12 シリンダ = **LBA 1632** から (`HDD_PARTITION_LBA`)。区画は**ディスク全体** | cdinst.c:30〜37, 111〜131, 216〜217 / install.c:28〜30, 251〜255 |
| F3 | 区画の終了シリンダは `total/136 - 1` を **16 ビット**で書く。F1 のディスクでは 121,426 → **桁あふれ**。8/17 で表せる上限は 65,536 × 136 = 約 4.5GB | 同上 |
| F4 | **カーネルは区画の開始 LBA を IDENTIFY の幾何で計算する** (`ext2_find_partition`: `(start_c × info.heads + start_h) × info.sectors + start_s`)。実機では 12 × 16 × 63 = **12,096**、インストーラが書いた実体は **1,632** → **マウントもフォーマットも別の場所を見る**。NP21/W の NHD は IDENTIFY も 8/17 なので一致して見えていた | fs/ext2_super.c:300〜339、fs/ext2_fmt.c:73 (`fmt_ctx.base_lba = ext2_find_partition()`) |
| F5 | IPL (`boot/boot_hdd.asm`) は IPL 内の `geo_heads/geo_spt` (インストーラが 8/17 を書く) で LBA→CHS を計算し **BIOS INT 1Bh** に渡す。**BIOS が 8GB ディスクをどの幾何で見せるかは未測定** (資料の壁: 8/17 = 4.25GB、16/63 = 31.4GB。Bible 2-9-1)。幾何は **INT 1Bh AH=84h (新センス)** で BX=セクタ長 / CX=シリンダ / DH=ヘッド / DL=セクタ として得られる (Bible 2-9 §3 SENSE [HDD]) | boot_hdd.asm:36, 70〜100 |
| F6 | `loader_hdd.asm` は IPL から DA / heads / SPT を `7F00h` のパラメータ域で受け取る。FD のローダ (`loader_fat_new.asm`) は HDD の幾何を問い合わせていない | loader_hdd.asm:24〜49、loader_fat_new.asm |
| F7 | ext2 の一括フォーマットの実績は 200MB の NHD まで (1KB ブロック、1 グループ 8192 ブロック)。シェルの `format [0-3] [sects]` は `ext2_format()` を呼ぶだけで区画表を書かない | fs/ext2_fmt.c、userland/shell/cmd_sys.c:112〜127 |
| F8 | カーネルの ATA I/O は IDENTIFY の幾何で LBA→CHS (`ide.c:356〜367`)。これは**ドライブ直叩き**なので IDENTIFY の幾何で正しい (BIOS の変換とは無関係) | drivers/ide.c |
| F10 | **区画表のバイト配置が PC-98 標準と 2 バイトずれている** (ラリー 1 で両者が指摘)。標準 (FreeBSD `diskpc98.h`、OS32 の `fs/fatfs_vfs.c:380` `PC98PartEntry`) は +4/5/6-7 IPL CHS、**+8 開始セクタ / +9 開始ヘッド / +10-11 開始シリンダ**、+12/13/14-15 終了。OS32 の書き手 (cdinst / install / `tools/nhd_deploy.py:689-723`) と読み手 (`ext2_find_partition`、`boot/boot_main.c:16-43`) は +6/+7/+8-9 を開始、+10〜13 を終了に使う独自配置で、**互いに揃っているので NP21/W では出なかった** | Codex B1 / Fable B2 |
| F11 | **ext2 は最大 32 グループ = 256MiB** (`fs/ext2_ctx.h:18` `EXT2_MAX_GROUPS 32`、format は `NOSPC`、mount は `IO`)。「200MB の実績」は設計上限の内側だっただけ | 両者 |
| F12 | formatter は**最終グループがメタデータより小さくても断らず区画外へ書く** (例: 200MiB を 8/17 のシリンダに切り上げると最終グループ 15 ブロック < 必要 249)。`ext2_find_partition` は読めない・見つからないとき **LBA 1088 を返し**、format はその位置から書き始める。`dev_blk_write_lba` は範囲検査をしない | Codex B3/B5、Fable 非 blocker 9 |
| F13 | カーネルの ATA CHS は IDENTIFY の**既定**幾何 (word 1/3/6)。BIOS が INITIALIZE DEVICE PARAMETERS で**現在の変換** (word 53 bit0 → word 54-58) を変えていれば、既定での CHS は別の物理セクタを指す。`lba_supported` (word 49 bit9) は読むだけで未使用 | Fable B3 |
| F14 | 低位メモリに「ブート情報の固定域」は**無い** (カーネルは `boot_drive` をスタック引数で受ける)。0x1000〜 はフォントキャッシュが上書きする。HDD ローダは INT 1Bh 後の CF を見ない (`loader_hdd.asm:158`) | Fable B4 / Codex B9 |
| F15 | 訂正: `install.c` は IPL に **IDENTIFY の幾何**を書く (区画表は 8/17) = 実機では混在 (F2 の誤り)。F3 の書かれる値は 55,889 (121,425 の下位 16 ビット)。F4 の後段: format も同じ `ext2_find_partition` を使うので、FS は 1632 ではなく 12,096 に作られようとする (その前にグループ上限で失敗) | 両者 |
| F9 | 実機で CD ドライブが検出されているかは未確認 (セカンダリは `bank1 probe=0x50`、`drive2/3 identify=-2` = ATAPI なら ATA IDENTIFY は断るので整合)。`dev` / `ls /cd0` をシリアルで依頼中 | 起動画面 |

## 1. 方針 v2 (ラリー 1 の合意を反映)

**原則**: (1) 幾何は 2 種類 — ATA 直叩きは**ドライブの現在の変換** (F13)、区画表と IPL の CHS は **BIOS 幾何**。(2) 区画表は **PC-98 標準配置** (F10) に直す。(3) **ディスクへの最初の書き込みの前に、成立条件を全部検査**し、1 つでも欠けたら 1 バイトも書かない。

### 段 0 — 実機の計測 (書き込み無し。段 1 の前に必ず)

1. FD ローダ (`loader_fat_new.asm`) が **INT 1Bh AH=84h** を DA=80h/81h に呼び、CF・AH・BX・CX・DH・DL を記録。置き場は**新設のブート情報域** (候補 0x8A000〜0x8BFFF、`include/memmap.h` に定義、`tools/gen_memmap.py` の表へ)。形式は magic + version + 反転チェック語 + drive ごとの valid + 取得元。カーネルは `kernel_main` の**フォント初期化より前**に写して保存し、以後その値だけを使う。magic が合わなければ「BIOS 幾何なし」。
2. カーネルは IDENTIFY の word 1/3/6 (既定)、word 53 と 54-58 (現在)、word 49 bit9 (LBA)、word 60-61 (総数) を表示。
3. 出力 `[hdd] bios da=80 cf=0 len=512 C/H/S=… / ata def=… cur=… lba=1 total=…` を実機で記録 (受入 H3)。**段 1 の設計値はこの結果で確定する** (8/17 か 16/63 か、既定 = 現在か)。

### 段 1 — 一時置き場 (FD 起動のまま)

4. **区画表の読み書きを 1 つの共有部に集約**し、標準配置で読み書きする: `ext2_find_partition`、`boot/boot_main.c`、cdinst、install、`tools/nhd_deploy.py`、fatfs の読み手と同じ struct を使う。**同じコミット**で揃える。既存 NHD は `make deploy-kernel` の区画表書き直しで移行する (H2 で確認)。旧配置を読む互換はしない (実機に OS32 の旧配置の区画は存在しない)。
5. **区画の探索は失敗を返す**: `ext2_find_partition` の 1088 フォールバックを廃止し、(start, length) を返す。format は検証済みの (start, length) だけを受け、その範囲外に書かない。
6. **大きさは既存の上限内**: 段 1 の既定は **256MiB 以下** (ext2 32 グループ)。最終グループがメタデータ (bitmap 2 + inode 表) を収められない長さは切り下げる。2GB への拡張 (`EXT2_MAX_GROUPS` を上げる、format の進捗表示) は**別票**。
7. **区画作成 `hdprep`** (シェル組込み、hd0 = DA 80h 専用、他は断る): 書く前に表示と検査 — 対象ドライブ・BIOS/ATA 幾何・書く LBA 範囲・LBA 0/1 の生の中身。**断る条件**: BIOS 幾何なし / BX≠512 / ATA の既定≠現在 (F13、ide.c を現在の変換か LBA28 に直すまで) / LBA 1 に空でない区画項目 or LBA 0 に 55AA (`--force` でも既存区画があれば断る。今回は空のディスク専用) / `/hd0` がマウント中 (先に umount し戻り値を見る)。実行は**ユーザーの [D2] 承認 + `yes` 入力**。書く前に予定域の末尾シリンダへ 1 セクタ write→readback の探り、区画表を書いたら読み戻して比較、format 後にマウント → 書き → 読み戻し。区画表は**最後に**書く (format 失敗で区画表だけ残らない)。
8. 起動時の `/hd0` マウントは段 1 の区画で働く (FD 起動時)。

### 段 2 — インストーラ

9. cdinst / install を段 1 の共有部 (区画表・幾何・検査) に乗せる。IPL の `[8]/[9]` には段 0 の BIOS 幾何を書く。区画開始 = LBA 1632 以上の最初の BIOS シリンダ境界 (8/17 なら 1632、16/63 なら 2016)。
10. 既存区画の再利用は**今回しない** (区画表を 2 回書く現行経路、`/sys` の mkdir 失敗、上書きの扱いが未整理 — Codex B7)。インストールは段 1 の区画を**作り直す** (その旨を確認画面で表示)。一時置き場のデータは失われることを明記。
11. 事前検査: パッケージの必須内容・ローダ 8192B 以下・展開先の容量。追加パッケージ・sync の失敗で「完了」と言わない。

### 段 3 — CD インストール → HDD 起動

12. HDD ローダは INT 1Bh ごとに CF を検査し、失敗を画面に出して止まる (F14)。
13. hd0 (DA 80h) 起動のみ。hd1 は断る。

## 2. ラリー 1 の論点の処理

| 論点 | 出所 | 処理 |
|---|---|---|
| 区画表の配置 | 両者 | 段 1-4 で標準へ。旧配置の互換はしない |
| 256MiB 上限 | 両者 | 段 1 は上限内、拡張は別票 (Fable 案 b = Codex 案) |
| 最終グループ・1088 フォールバック・範囲検査 | Codex | 段 1-5/6 |
| 現在の変換 (word 53-58) | Fable | 段 0-2 で測り、既定≠現在なら書き込みを断る |
| ブート情報域の置き場・偽陽性 | 両者 | 段 0-1 |
| 既存データ・使用中媒体・承認 | 両者 | 段 1-7 (空のディスク専用) |
| 再利用 | Codex | 段 2-10 でしない |
| drive / DA / mount の対応 | 両者 | hd0 = 80h 専用 |
| ローダの CF / 事前検査 / 完了条件 | 両者 | 段 2-11、段 3-12 |
| IPL に幾何を焼く方式の陳腐化 | Fable 非 blocker 4 | 記録のみ (IPL 自身が AH=84h を呼ぶ案は 512B 制約と合わせて段 3 で再検討) |

## 3. 受入 (案 v2)

| ID | 内容 | 場 |
|---|---|---|
| H1 | ホスト試験: 共有部の区画表 (標準配置、`PC98PartEntry` と一致) の書き→読み、8/17 と 16/63、32/33 グループ境界、最終グループ切り下げ、16 ビットシリンダ超過・幾何なし・範囲外の拒否、IPL の値 = 区画計算の幾何 | ホスト |
| H2 | NP21/W: `deploy-kernel` 後の既存 NHD が新配置で起動・マウント、kselftest。FD 起動 (HDD 無し) で `[hdd] bios da=80 cf=1` → 幾何なし → 従来どおり | NP21/W |
| H3 | 実機: 段 0 の `[hdd]` 行を記録 | 実機 |
| H4 | 実機: 承認後 `hdprep` → 再起動 → `/hd0` マウント → 書き → 再起動後 md5 一致 (区画の前半と末尾の両方のファイル) | 実機 |
| H5 | 実機: CD インストール → HDD 起動 → kselftest | 実機 |

## 4. しないこと

8GB 全体・2GB 区画 (別票)、複数区画・既存区画との共存・再利用、hd1 起動、4KB ブロック、LBA 拡張 BIOS。
