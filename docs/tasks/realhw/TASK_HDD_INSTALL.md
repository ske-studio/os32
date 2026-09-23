# TASK_HDD_INSTALL — 実機 Ra266 の 8GB IDE へ置き場を作り、CD からインストールして HDD 起動する

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23) / 状態: **方針レビュー (Fable + Codex、ラリー 1)**。実装はキーボード修正 (wt/kbd-rty) の着地後。
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
| F9 | 実機で CD ドライブが検出されているかは未確認 (セカンダリは `bank1 probe=0x50`、`drive2/3 identify=-2` = ATAPI なら ATA IDENTIFY は断るので整合)。`dev` / `ls /cd0` をシリアルで依頼中 | 起動画面 |

## 1. 方針 (レビュー対象)

**幾何は 2 種類あり、混ぜない** — (a) **ドライブの幾何** (IDENTIFY、ATA 直叩きの CHS 用、F8)、(b) **BIOS の幾何** (INT 1Bh が使う変換後の CHS。区画表と IPL の CHS はこちらで書かれていなければ BIOS の起動メニュー・IPL が読めない)。**区画表の CHS ⇄ LBA の変換は (b) だけで行う**。

### 段 1 — 一時置き場 (FD 起動のまま、起動の問題と切り離す)

1. **BIOS 幾何の取得**: FD のローダ (`loader_fat_new.asm`、実モード) が起動時に **INT 1Bh AH=84h を DA=80h (と 81h)** に対して呼び、結果 (成否、セクタ長、C/H/S) を低位メモリの固定域 (`include/memmap.h` に新設、既存のブート情報の並び) に置く。カーネルは起動時にそれを保存し、`[hdd] bios geom da=80 C/H/S/len` を表示する。**取れなかったら「BIOS 幾何なし」**として、段 1 の区画作成は断る (推測で 8/17 にしない)。HDD 起動時は IPL が 7F00h で渡している heads/SPT を同じ固定域に写す (loader_hdd 側)。
2. **区画の開始 LBA の計算を (b) に**: `ext2_find_partition()` は BIOS 幾何で CHS→LBA を計算する。BIOS 幾何が無い場合 (NP21/W の古い起動経路など) は従来どおり IDENTIFY の値 (NP21/W では一致するので後方互換)。
3. **区画作成コマンド** (シェル組込み `hdpart` か独立の `/sbin/hdprep.bin`): `hdprep <drive> <MB>` で区画表 (LBA 1) に**1 区画** (bootable、ext2、開始 = シリンダ境界に揃えたブート予約の後、長さ = 指定 MB を BIOS 幾何のシリンダ境界に丸め)、続けて `ext2_format`。**既定の大きさは 2GB** (ユーザー未指定のため PM の提案。上限は BIOS 幾何で表せる範囲と 16 ビットのシリンダ)。IPL は書かない (起動は FD のまま)。**実行はユーザーの [D2] 承認の後** (実機ディスクへの初回書き込み)。確認プロンプト (ドライブ・容量・既存の区画表の有無を表示し `yes` を要求) を付ける。
4. **マウント**: 起動時に hd0 の ext2 区画を `/hd0` にマウントする既存の経路が段 1 の区画で働くこと (FD 起動時でも)。

### 段 2 — インストーラを直す

5. `cdinst.c` / `install.c` の固定幾何 (8/17) を**BIOS 幾何**に置き換え、区画の大きさに段 1 と同じ上限・指定を入れる (区画表の書き手は 1 か所の共有関数に寄せる)。IPL の `ipl[8]/[9]` にも BIOS 幾何を書く。ブート予約の大きさ (LBA 1632 = 12 シリンダ × 136) は **LBA で固定**し、区画の開始をその後の最初のシリンダ境界にする (`tools/nhd_deploy.py` の `HDD_PARTITION_LBA` との一致規則を LBA ベースに直す)。
6. 段 1 で作った区画があれば**消さずに使う**選択肢 (再フォーマットするか訊く)。

### 段 3 — CD からインストールして HDD 起動

7. FD で起動 → CD (`/cd0`) の `cdinst` でインストール → FD を抜いて HDD 起動。起動しなかったら段 1 の置き場は残る (区画表を作り直さない限り)。

## 2. 決めてほしいこと (レビュアーへ)

- Q1: BIOS 幾何を FD ローダで取る案 (実モードで INT 1Bh AH=84h) で足りるか。V86 (`v86`) 経由でカーネルから呼ぶ案、IDENTIFY から BIOS の変換規則を推定する案と比べて。
- Q2: `ext2_find_partition` を BIOS 幾何に変えたときに、既存の NHD (NP21/W、8/17) と CI の FD 起動で壊れる経路は無いか。
- Q3: 2GB の ext2 (1KB ブロック、約 256 グループ) を `ext2_format` で作るとき、既存コードに 32 ビット・静的バッファ・グループ数の上限 (ext2_g_aux など §4-24) の問題は無いか。4KB ブロックにすべきか。
- Q4: ブート予約を LBA 1632 固定にし、区画開始を BIOS 幾何のシリンダ境界に切り上げる規則は、PC-98 の IPL / BIOS 起動メニューの前提に反しないか。
- Q5: 段 1 の区画作成を実機で安全に行うための確認・中止の条件は足りているか。

## 3. 受入 (案)

| ID | 内容 | 場 |
|---|---|---|
| H1 | ホスト試験: 区画表の書き手 (BIOS 幾何・大きさ → 区画表 32 バイト) と `ext2_find_partition` の計算が 8/17 と 16/63 の両方で一致、終了シリンダが 16 ビットに収まらない指定を断る | ホスト |
| H2 | NP21/W: 既存 NHD の起動・マウントが変わらない (kselftest、`ls /`)。FD 起動で `[hdd] bios geom` が出る | NP21/W |
| H3 | 実機: `[hdd] bios geom da=80 …` の値を記録 | 実機 |
| H4 | 実機: `hdprep 0 2048` (承認後) → 再起動 → `/hd0` にマウントされ、ファイルを書いて再起動後に md5 一致 | 実機 |
| H5 | 実機: CD インストール → HDD 起動 → kselftest | 実機 |

## 4. しないこと

8GB 全体を 1 区画にする、複数区画の管理 (fdisk 相当)、FAT 区画との共存、LBA 拡張 BIOS の利用。
