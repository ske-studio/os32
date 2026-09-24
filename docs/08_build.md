## 第8部 ビルドシステム (GCC/NASM/Make)

### §8-1 ビルド手順

```bash
# 全体ビルド (カーネルおよびプログラム, 統合環境イメージ images/os32_boot.d88 等を生成)
make all

# パッケージ生成 + ISOイメージ生成 (CDインストーラ用)
make iso

# 掃除
make clean
```

### §8-2 ビルドパイプライン

```
=== カーネルビルド ===
boot/*.asm      →  nasm (-f bin / -f elf32)  →  boot/*.bin / boot/*.o
kernel/*.asm    →  nasm (-f elf32)           →  kernel/*.o
*/*.c           →  gcc (-m32 -ffreestanding) →  */*.o
                                               ↓
*.o + kernel/*.o → ld (-T os32.ld)           → build/out/kernel.elf (0x100000〜 + SQLite 0x200000〜)
                                               (+ build/out/kernel.map)
kernel.elf      →  objcopy (-O binary)       → build/out/kernel.bin (カーネル本体)
                →  objcopy (--only-section)  → build/out/sqlite.bin (SQLite拡張域)
                                               ↓
kernel.bin + sqlite.bin → mkvmkernel.py      → build/out/vmkernel.lz4 (LZ4圧縮カーネルイメージ)
                                               LZ4 高圧縮 (HC level 12、展開側は同じ形式で無変更)。
                                               合計が MAX_IMAGE_SIZE (boot/boot_defs.h、508KiB) を
                                               超えたら出力を消して失敗 (HDD ローダが読めない)。
                                               試験: make check-vmkernel-lz4-host

※ カーネル関連のビルド成果物はすべて `build/out/` に集約される (`BUILD_OUT`、gitignore対象)。
  `tools/gen_unicode` の出力 `unicode.bin` も同ディレクトリへ移動される。

=== HDD デプロイ ===
boot_hdd.asm    →  nasm (-f bin)             →  boot_hdd.bin (512B IPL, LBA 0)
loader_hdd.asm + boot_main.c + ext2_mini.c + lz4_mini.c
                →  nasm + gcc + ld (-T loader.ld) → loader_hdd.bin (LBA 2-5)
build/out/vmkernel.lz4 → ext2 FS 内 /boot/vmkernel.lz4 に配置 (nhd_deploy.py)

=== FDD デプロイ (FAT12) ===
boot_fat.asm    →  nasm (-f bin)             →  boot_fat.bin (1024B FAT12 IPL)
loader_fat_new.asm →  nasm (-f bin)          →  loader_fat_new.bin
mkfat12.py --tree で FAT12 イメージを構築:
  /LOADER.BIN = loader_fat_new.bin, /VMKRNL.LZ4 = build/out/vmkernel.lz4,
  /sys/*.bin + /bin/*.bin (FDD_MIN_CMDS) を配置
                                             ↓
                                    images/os32_boot.d88 および .img

=== 外部プログラム ===
userland/**/*.c → gcc -m32                   → *.o
                → ld -T sdk/link/app.ld      → *.elf (newlib-nano -lc -lgcc リンク)
                → objcopy                    → *.raw
                → sdk/mkos32x.py             → *.bin (OS32X ヘッダ付き)

apps/ と game/ は staged SDK (build/sdk/) だけを使い、それぞれの
Makefile が同じ流れを回す。OS のソースツリーは参照しない。

=== GUI シェルと共有ライブラリ (2026-09-06) ===
userland/gshell (Rust, cargo)  → libgshell.a
  + crt0 + libos32gfx (GFX_OBJ) → ld -T sdk/link/app_sys.ld → userland/gshell.bin (シェル帯 0x300000、make gshell)
userland/rust/libos32gui       → ld -T sdk/link/shlib.ld     → userland/libos32gui.elf/.raw
                → tools/mkshlib.py --api 42 → userland/libos32gui.shlib (/sys/lib、make shlib)
                   (make check-shlib = 番号表の突き合わせ。ジャンプ表は末尾追記のみ)
GUI アプリ      → libos32gui_stub (ジャンプ表への薄いスタブ) を静的リンク、libos32gfx は入れない
```

日常のターゲット (`make all` に含まれる): `kernel` `programs` `libs` `gshell` `shlib` `external`
(`apps` + `game`)。検査: `make check` (= `check-kapi-version` `check-manifests` `check-constraints`
`check-privileged` `check-ne2000-ring` `check-shlib` `check-gui-proto`)。`emu_agent` (ローカル AI) の `make` は
許可リスト (`tools/emu_agent/agent.py` の `MAKE_TARGETS`) に載ったターゲットしか実行しない。

**GitHub Actions** (`.github/workflows/check.yml`、2026-09-06): push / PR で、クロスツールチェーン無しで
回せる検査だけを自動ゲートにする — KAPI 版番号の一致、`sdk/kapi.json` からの生成物がコミット済みと
一致すること ([ABI1])、CONSTRAINTS ⇄ CLAUDE.md、`mkshlib --check`、GUI プロトコルの C ⇄ Rust 照合
(`check_gui_proto.py`)、ne2000 リングのホストテスト。
`check-manifests` は `make all` の成果物を見るので対象外 (WSL 側の `make check` で回す)。

**コンパイラとフラグ** (実体は `build/config.mk`。ここは読むための写しで、値は config.mk が正しい):

| 対象 | コンパイラ | 主なフラグ |
|---|---|---|
| カーネル | i386-elf-gcc | `-std=gnu89 -m32 -march=i386 -ffreestanding -fno-pie -fno-stack-protector -O2` |
| SQLite | i386-elf-gcc | 上記 + `-Os -ffunction-sections -fdata-sections` (サイズ優先) |
| 外部プログラム | i386-elf-gcc | 同じ基本フラグ + `sdk/link/app.ld` でリンク |
| アセンブラ | NASM | `-f elf32` (カーネル) / `-f bin` (ブートセクタ) |

クロスコンパイラは `$CROSS_DIR` (既定 `/usr/local/cross`)。構築手順は §8-5。

インクルードパスは `Makefile` で細かく制御されており、基本的にソースファイルから他のヘッダディレクトリは `-I` によって自動解決できるため `#include "file.h"` で問題なく参照可能。

### §8-3 ディレクトリ構造

```
os32/
├── boot/           ブートローダー (16bit/32bit ASM + C)
├── kernel/         カーネルコア・メモリ等・割り込みルーチン群 (gui.c / shlib.c / sysconfig.c / ring3_entry.asm を含む)
├── drivers/        ハードウェアドライバ (kbd, rtc, fm, fdc, disk, ide, atapi, kcg, mouse, np2sysp, loop_dev, dev, wab_* (Cirrus / Xe10 グルー) 等)
├── gfx/            グラフィック HAL (gfx_core + backend_pc98 / backend_pegc / backend_cirrus)
├── fs/             ファイルシステム (vfs, ext2, fatfs, iso9660, hostdrv 等)
├── exec/           OS32X(外部プログラム) のロードと環境設定
├── kapi/           外部プログラム向け KernelAPI リダイレクタ
├── lib/            汎用ライブラリ (utf8, path, sqlite3, zlib, microtar 等。vendor したものは各ディレクトリの README.OS32 が出所とライセンスの正典)
├── include/        システム統合用共通ヘッダ群 (memmap.h, gfx_hal.h, wab_xe10.h 等)。io.h / cpu.h は原始命令の**契約**だけ
├── arch/           CPU 依存の実装 (x86/arch_io.h — 割り込み制御・CPU 停止・IDT ロード、x86/arch_cpu.h — CR3/CR0・リング降下、x86/x86_desc.h — GDT/TSS ロード)。`ARCH ?= x86` で選ぶ。足し方は arch/README.md
├── platform/       機種依存の実装 (pc98/platform_io.h — ポート I/O・I/O ウェイト)。`PLATFORM ?= pc98` で選ぶ
├── userland/       ユーザー空間 (shell/, gshell/ (GUI シェル, Rust), cmds/, system/, tests/, rust/ (libos32gui 等), lib/)
├── .github/        GitHub Actions (workflows/check.yml: 静的ゲート)
├── apps/           git submodule (ske-studio/os32-apps) — 標準アプリ。make external / make apps
├── game/           git submodule (ske-studio/os32-game) — 対戦スゴロク RPG。make external / make game
├── docs/hw/        PC-98 資料のローカルミラー (git 管理外、tools/sync_hwdocs.sh)
├── sdk/            配布 SDK (include/, crt/, link/ (app.ld / app_sys.ld / shlib.ld), rust/, example/)、kapi.json と生成器
├── build/          モジュール化 Makefile 群 (config.mk, kernel.mk, programs.mk, libs.mk, deploy.mk, image.mk, sdk.mk 等) + リンカスクリプト
│   └── out/        ビルド成果物 (kernel.bin, sqlite.bin, vmkernel.lz4, unicode.bin, kernel.elf/.map)
├── assets/         データアセット (DB, 辞書, profile 等)
├── tests/          テストスクリプト
├── tools/          ホスト上でのイメージ生成・デプロイ・検査ツール (nhd_deploy, mkshlib, check_*, emu_agent/ (ローカル AI の実機操作), np21w_mcp/)
├── packages/       生成された .PKG (make packages)
├── images/         生成されたブートイメージ (make all / iso)
├── Makefile        マスタービルドスクリプト (build/*.mk を include)
├── .env            ローカル環境設定 (CROSS_DIR / NP21W_DIR / HOSTDRV_DIR)
└── docs/           仕様書ドキュメント群
```

### §8-4 ホスト側イメージ生成ツール

#### `tools/nhd_deploy.py` (現行の標準デプロイツール)
NHD HDDイメージの管理をホスト側で一元化するデプロイメントツール。`deploy.yaml` に基づき、カーネル・ローダー・全ファイルを一括デプロイする。

```bash
python3 tools/nhd_deploy.py init       # NP21/WからNHDをコピー+パーティション更新+フォーマット+マウント
python3 tools/nhd_deploy.py sync       # deploy.yaml に基づくフルデプロイ
python3 tools/nhd_deploy.py write-boot boot/loader_hdd.bin  # ブート領域書き込み
python3 tools/nhd_deploy.py sync-from-hostdrv  # HostDrv (C:\os32) から ext2 へ同期
python3 tools/nhd_deploy.py deploy     # ローカルNHD (build/nhd/os32.nhd、無ければ自動 pull) をNP21/Wにコピー
python3 tools/nhd_deploy.py copy userland/shell.bin  # 個別ファイルのデプロイ
make deploy                                                # HostDrv 同期 → ゲストで hsync (再起動不要)
# 他: mount / umount / ls / rm / mkdirs / format / write-boot
```
- 配備対象・ゲストパス・タグは層ごとの deploy.yaml で定義する
  (`build/core.yaml`, `userland/`, `apps/`, `game/`)。マージは `tools/deploy_manifests.py`
- ext2ファイルシステムへの書き込みはLinux loopデバイス経由
  (sudo NOPASSWD 推奨: `mount, umount, losetup, e2fsck, mkfs.ext2, mke2fs, cp, mkdir, rm`)
- `config.h` の `SYS_*` 定数と `deploy.yaml` のパスは必ず整合させること

> ⚠️ **NP21/W 実行中は `deploy` が反映されない**: NP21/W が os32.nhd を開いたままの
> 状態ではコピーが失敗またはサイレントに無効化される。**必ず
> `taskkill.exe /F /IM np21x64w.exe` → `make deploy-kernel` → `np21w_restart.py`
> の順で実行**し、デプロイ後は `ver` の Build タイムスタンプで反映を確認すること
> (POLICY_DEBUG.md §2 / §4-9)。

<a id="ビルドターゲット"></a>
Makefile ターゲットとの対応 (`build/deploy.mk`)。**このリポジトリで
ターゲット一覧の正典はこの表**で、他のドキュメントはここを指すこと:

| ターゲット | 動作 |
|-----------|------|
| `make fd144` | **1.44MB フロッピーイメージ** `images/os32_boot144.img` (生イメージ、1,474,560 バイト)。2HD の `images/os32_boot.d88` とは別物で、既定は 2HD のまま。票 [`tasks/realhw/TASK_FD144.md`](tasks/realhw/TASK_FD144.md) |
| `make deploy` | HostDrv (`C:\os32`) への同期 — 再起動不要 |
| `make deploy-kernel` | HostDrv同期 + HostDrv→ext2同期 + NHDコピー — **要NP21/W再起動**。名前に反して**カーネル単独ではなく一式** (ユーザーランド・`/sys` も NHD へ書く) |
| `make deploy-boot` | ブートローダー (loader_hdd.bin) をNHDブート領域へ書き込み |
| `make deploy-nhd` | deploy.yaml フルデプロイ + NHDコピー — **要NP21/W再起動** |
| `make prune-stale` / `make prune-stale-delete` | 配備先 (HostDrv + NHD) に残ったマニフェストに無い *.bin を一覧 / 削除。deploy 系は既定で削除まで行う (`NO_PRUNE=1` で一覧のみ) |
| `make apps` / `make game` | 外部リポジトリ (git submodule `apps/` = os32-apps、`game/` = os32-game) を SDK 経由でビルド。空なら `git submodule update --init` を促す |
| `make external` | 上記 2 つをまとめて。KAPI / SDK ライブラリ変更後に再ビルドする。ポインタ更新条件は下記参照 |
| `make clean-external` | 外部リポジトリの生成物を削除 |
| `make deploy` → ゲストで `hsync` | HostDrv 経由の配送 — 再起動不要。`hsync` は既定で `/sys` を外す (稼働中のシェル・共有ライブラリ)。入れ替えるときは `hsync sys` |
| `make nhd-pull` | Windows 側 NHD を作業イメージ `build/nhd/os32.nhd` に取り込む (フォーマットしない)。deploy 系は無ければ自動で pull する |
| `make nhd-init` | 初回セットアップ — **フォーマットするのでゲスト側データが消える** |
| `make nhd-mount` / `make nhd-umount` | 作業イメージの手動マウント・アンマウント |

ビルド側のターゲットは `make all` / `kernel` / `libs` / `programs` / `sdk` /
`apps` / `game` / `clean` / `clean-kernel` / `clean-libs` / `clean-programs`。
KernelAPI の構造体を変えたときは `make clean` → `make all` が必須
(古い `.o` が残ると ABI 不整合で静かに壊れる)。

<a id="kapi-v63-移行"></a>
#### KAPI v63 への移行 (データ欄の固定配置、票 [TASK_KAPI_DATA_FIELDS](tasks/memory/TASK_KAPI_DATA_FIELDS.md))

v63 で KernelAPI のデータ欄を 0x4B8 に固定し、OS32X ヘッダを v3 にした
([KAPI_SPEC.md](KAPI_SPEC.md) §4-0)。**v62 以前のバイナリ (アプリ・常駐シェル・
`libos32gui.shlib`) は一度だけ全部断られる**ので、初回は次の順で入れ替える:

1. `make clean && make clean-external` — 古い `.o` は crt の `kapi` の改名
   (`os32_kapi_v63`) でリンクが落ちるが、`.bin` は残るので必ず消す
2. `make all external` (実機向けは `make fd144` も) — 全部ヘッダ v3 で作り直す
3. **NHD は NP21/W を止めて一式** (`make deploy-nhd`、カーネル・`/sys`・shlib・
   userland をまとめて。順序は問わない)。実機は **FD / CD を入れ直す**
4. HostDrv + `hsync` だけでは移れない — 旧 `hsync` は名札の `kapi=` を見ないし、
   `/sys` (常駐シェル) が旧いまま新カーネルに載ると起動時に止まる
   (`FATAL: shell.bin: rebuild required (KAPI data layout)`、FDD の shell があればそちら)

**v64 以降は「カーネルを先、ユーザーランドを後」** (ユーザー決裁 2026-09-24)。
データ欄が固定になったので、新しいカーネルは古いユーザーランドをそのまま動かせる。
逆 (新しいユーザーランド + 古いカーネル) は、`min_api_ver` (exec と shlib ローダ) と
`hsync` の「配備物の版 > カーネルの版」の拒否 (`reason=kapi_newer_than_kernel`) が止める。

HostDrv 経由 (NP21/W を止めない) で v63 以降の稼働機を v64 以降へ上げる手順:

1. ホストで `make all external` → `make deploy` (HostDrv に一式と名札 `kapi_version=64…`)
2. ゲストで **`hsync boot`** — `/boot` だけに絞った同期は「版が新しい」の拒否から
   外してある (`NOTE: /boot だけの同期なので…` が出る)。配置違い
   (`kapi_layout_mismatch`) と名札の欠落・不正は `/boot` でも断る
3. 再起動 (`NOTE: /boot を更新した -> 再起動が必要`)
4. **`ver` の `API: v64` で版を確かめる** — 上がっていなければここで止まる
   (次の `hsync` は `kapi_newer_than_kernel` で断るので、壊れはしない)
5. `hsync` (ユーザーランド)、続けて `hsync sys` (常駐シェル・`libos32gui.shlib`)
   → shlib を替えたらもう一度再起動

`/boot` 以外 (`hsync bin`、全体同期、`/bootx` のような似た名前) は従来どおり断る。
**`make deploy-kernel` はカーネル単独の配備ではない** — HostDrv 同期の後に
HostDrv の中身 (ユーザーランド・`/sys` を含む一式) を NHD の ext2 へ書き、
NP21/W の停止が要る ([D1])。停止できるならこちらで一式を入れても順序の問題は出ない。

<a id="配備3経路"></a>
#### 配備 3 経路の使い分け (正典)

| 経路 | コマンド | 何が起きるか | 再起動 |
|---|---|---|:---:|
| **HostDrv** | `make deploy` | 成果物を `C:\os32` へ同期。ゲストは `/host` マウントで読む。速い反復用 | 不要 |
| **NHD** | `make deploy-kernel` | HostDrv 同期 + カーネル・プログラム・データを NHD の ext2 へ丸ごと書く | **必要** |
| **ブートセクタ** | `make deploy-boot` | `boot/loader_hdd.bin` を NHD のブート領域 (LBA 2〜17) へ。ローダを変えたときだけ | **必要** |

- **NHD への書き込みは NP21/W を止めてから** ([D1])。停止 → 配備 → 起動の順。
  `emu_pause`、breakpoint 停止、HTTP 無応答はプロセス終了の証拠にならない。
- **HostDrv だけでは検証にならない** ([V1])。ゲストの PATH は NHD の `/usr/bin` を先に見るので、
  古いバイナリが黙って動き、合格したように見える。
- ユーザーランドは `make deploy` → ゲストで `hsync` で再起動なしに差し替えられる
  (ホットデプロイの物理末尾 256KB 窓は 2026-09-09 に撤去)
  (カーネルと `/sys` は不可)。
- 配備マニフェストは所有層ごとに分かれている (`build/core.yaml`、`userland/deploy.yaml`、
  `apps/deploy.yaml`、`game/deploy.yaml`)。統合は `tools/deploy_manifests.py`。
  マニフェストに無いバイナリは配備先で stale 化するので、`make deploy*` が
  `tools/prune_stale.py` で刈る (`NO_PRUNE=1` で一覧のみ)。
- **配備元には世代の名札が付く** (票 H4)。`make deploy` は全件成功の後にだけ
  `C:\os32\.deploy\manifest.txt` を書き、1 件でも失敗したら既にある名札を消す。
  中身は行指向の平文 (`format` / `build` / `generated` / `kapi` / `kapi_version` / `count`
  + `---` + 1 行 1 ファイル)。format=2 (KAPI v63〜) の `kapi=` は配備した OS32X バイナリの
  ヘッダ v3 から取る配置 (10 進) で、v3 でないもの・値の食い違うものが 1 つでもあれば
  名札を書かない (= 配備失敗)。
  `build` は `<短い SHA>`(+`dirty`) で、**「同じか違うか」を見るための名札。順序は表さない**。
  ゲストの `hsync` は起動時にこれを読んで 1 行目に `DEPLOY build=… count=… generated=…` を出し、
  `hsync --expect-build <ID>` は名札が違えば**1 件も書かずに**断る
  (`reason=build_mismatch`)。名札が無い / 壊れているときも「一致」とは扱わず、
  全体同期では断る (`manifest_absent` / `manifest_invalid`)。範囲を絞った同期
  (`hsync usr`) では表示だけして続ける — 名札はルートの世代を表すもので、
  絞った範囲の正しさは保証しないため。**読めて不一致と分かった場合は絞り込み
  でも断る** (「確かめた結果おかしい」と「確かめられない」は別)。
  **KAPI の門** (v63〜): 名札の `kapi=` がカーネルの配置と違う
  (`kapi_layout_mismatch`)、`kapi_version` がカーネルより新しい
  (`kapi_newer_than_kernel`)、名札が無い・壊れている・旧形式 (format=1) のときは、
  `--expect-build` の有無にも範囲にもよらず**既定で 1 件も書かずに断る**。
  越えるのは `--force-kapi` だけ (`-f` は同一判定の省略で別の意味なので開けない)。
  防ぐのは **「`make deploy` を忘れたまま `hsync` して、ゲストの新しいファイルを
  ホストの古いもので上書きする」** 事故 — 内容の違いは内容比較で分かるが、
  どちらが意図した版かは分からないため。詳細は `docs/manpages/hsync.1`。
  調査で名札を書かせたくないときは `hostdrv_deploy.py sync --no-manifest`
  (このときと `--tag` の部分配備では、**古い名札も消す**)。
  **名札が消えると、以後の `hsync` は KAPI の門で `manifest_absent` になり
  `--force-kapi` 無しでは 1 件も書かない** — 戻すには `make deploy` を通しで打つ。
  なお**コピーと名札の更新は原子的ではない**: 全件コピーの後・名札を書く前に
  ホストが落ちると、配備元は新しいのに名札は古いままになる。`--expect-build` は
  そこで断る (安全側)。復旧は `make deploy` をもう一度打つだけ。
- 環境変数: `HOSTDRV_DIR` (既定 `/mnt/c/os32`)、`NP21W_DIR` (既定 `/tmp/np21w`)。
- 判断と検証の進め方はスキル `os32-build-verify`、反映確認の手順は
  [POLICY_DEBUG.md §2](POLICY_DEBUG.md)。

#### submodule (`apps/` `game/`) の扱い

標準アプリとゲームは別リポジトリ (`ske-studio/os32-apps` / `ske-studio/os32-game`) で、
`apps/` `game/` に git submodule として置き、`make sdk` が作る `build/sdk/` を指してビルドする。

```bash
git submodule update --init     # 初回 / clone 直後
make external                   # apps + game (make apps / make game で個別)
```

- **KAPI を動かしたら `make external` で両方を再ビルドする。**
  再ビルドだけでは submodule のコミットもポインタも変わらない。submodule 側のソース変更を
  コミットして参照先が変わった場合にだけ、検証した組み合わせのポインタを親リポジトリで更新する。
  コミット・push はユーザーの明示的な指示がある場合のみ行う。
- **SDK のライブラリ (libos32gfx 等) を変えたときも同じ。** アプリは静的リンクなので、
  古い `.bin` は新しいバックエンド (PEGC の PACKED8 等) で #PF する
  (2026-09-06 に hello32 で実測)。

#### `tools/hostdrv_deploy.py`
HostDrv デプロイ先 (`HOSTDRV_DIR`, 既定 `C:\os32`) への差分同期。sudo 不要で高速。`make deploy` から呼ばれる。

#### `tools/mkpkg.py`
OS32パッケージ (.PKG) を生成するビルダー。複数ファイルをLZSS圧縮し、hash-chainで連結したパッケージを生成する。CDインストーラ (`cdinst.bin`) と連携し、ISOイメージ経由でのプログラム配布に使用される。

```bash
python3 tools/mkpkg.py --defs tools/package_defs.yaml --output packages/ --base .
```
- `make packages` ターゲットで自動実行 (定義: `tools/package_defs.yaml`)
- `make iso` で `genisoimage` を使用しISOイメージを生成

#### `build/app.conf` (OS32X ヘッダ設定)

`sdk/mkos32x.py` に渡す引数をプログラムごとに決めるデータファイル。`build/programs.mk` の
`userland/%.bin` ルールが `awk` で引き、キーはリポジトリルートからの拡張子なしパス
(例: `userland/tests/blit_test`)。`#` 始まりはコメント、行が無いプログラムは既定値。

| 列 | 意味 | 省略時 |
|---|---|---|
| 1 | プログラム名 (キー) | — |
| 2 | 要求 KAPI バージョン (`--api`) | 7 |
| 3 | ヒープサイズ (`--heap`)。`0` で mkos32x の既定 | 0 |
| 4 | `gfx` = 全画面 GFX の宣言 (`--gfx` → `OS32X_FLAG_GFX` = 0x0001)<br>`cui` = CUI 専用の宣言 (`--cui-only` → `OS32X_FLAG_CUI_ONLY` = 0x0010)<br>`launcher` = 起動要求者の宣言 (`--launcher` → `OS32X_FLAG_LAUNCHER` = 0x0020) | 無し |

4 列目は宣言ビットで、`gfx` / `cui` / `launcher` のどれか 1 つ (または省略)。

`gfx` は「このプログラムは画面を丸ごと取る」という宣言 (票 T8 D1a)。`gfx_init` /
`gfx_init_200` を呼ぶプログラム — 直接でも `tilemap_init` のようにライブラリ経由でも — に立てる。
`libos32gfx_attach` だけで gshell の面に取り付く GUI アプリには立てない。立て忘れると GUI 中の
起動で画面の所有権を取れず、WM が上書きするか カーネルが `gfx_init` を蹴る (蹴られたアプリは
そのまま畳まれる — 断っただけでは描画 KAPI で描き続けて GUI を壊すため)。

`cui` は「GUI から起動してはいけない」という宣言 (票 T8-2)。`v86_selftest` / `v86_disktest` /
`v86_boot` / `v86_boot2` を呼ぶ V86 / VDM 系に立てる。これらは CPL=3 のプログラムだが KAPI の
向こうで低位メモリ・BIOS・テキスト VRAM を丸ごと使うので、`--cpl0` の砦では捕まらない
(`userland/cmds/v86.bin` の flags は 0x0 だった = 受入 F5 の不合格)。GUI からの `exec_start` は
`OS32_ERR_INVAL` で断り、CUI からは従来どおり通す。グラフィック VRAM を直接書く検証用
バイナリ (`ring3_hello` / `ring3_fault` / `ring3_guard`) は app.conf を持たないので
`build/programs.mk` の explicit ルールで `--cui-only` を付けている。

`launcher` は「このプログラムは `launch_req` で WM に外部プログラムの起動を頼む」という宣言
(票 T9 D1a)。カーネルの要求表はこの宣言を持たない CPL=3 からの `launch_req` を `OS32_ERR_INVAL`
で断る (認証ではなく協調的な宣言)。端末 (`userland/tests/t5a_display`) と
`userland/sh` — 常駐シェルと同じソースを `-DSHELL_AS_APP` で 0x500000 にリンクした CPL=3 版 — に
立てる。GUI アプリの通常の起動経路 (`session_launch`) はこの宣言と無関係。

`gfx` / `cui` は `make check-manifests` がソースの呼び出しと突き合わせて検出する (§2b)。
`launcher` は同じ §2b が書式 (4 列目に置けるのは 3 つの印か省略) だけを見る。
`apps/` `game/` は staged SDK 側でそれぞれの `Makefile` が `mkos32x` を呼ぶので、
そちらの GFX プログラムには各リポジトリで `--gfx` を付ける。

#### `tools/audit_cast_align.sh`
非整列アクセス候補の洗い出し (他アーキテクチャ移植の事前監査)。ホストの `gcc -m32` と
`-Wcast-align=strict` で「アラインメント要件を上げるポインタキャスト」を列挙する。
i386-elf クロスコンパイラは不要、`-fsyntax-only` なので成果物も作らない。`make check` には組み込んでいない。

```bash
tools/audit_cast_align.sh kernel   # kernel/ drivers/ gfx/ fs/ exec/ kapi/ lib/
tools/audit_cast_align.sh user     # userland/ (newlib ヘッダが要るため網羅率は低い)
```
警告が出た = 必ず壊れる、ではない。仕分けの手順と結果は
[tasks/arch_port/M0_PORTABILITY_AUDIT.md](tasks/arch_port/M0_PORTABILITY_AUDIT.md)。

#### `tools/check_le_access.py`
外部形式 (媒体・書庫の上に並ぶバイト列) への直アクセスの番人 (移植準備の順序 4-a、
`make check` の `check-le-access`)。対象は ext2 / ISO9660 / KCG フォント書庫を読み書きする
9 ファイルで、`*(u32 *)&buf[off]` の形が無いことを見る**文字列検査**と、実ビルドと同じ
`i386-elf-gcc` に `-Wcast-align=strict` を足して単体コンパイル (`-fsyntax-only`) し警告 0 で
あることを見る**コンパイル検査**の 2 段。文字列検査をすり抜ける書き方
(`u32 *p = (u32 *)buf;` と 2 行に分ける等) は後段が捕まえる。外部形式は
`include/endian_le.h` の `le16_rd` / `le16_wr` / `le32_rd` / `le32_wr` を通すこと —
直アクセスは「x86 は LE」「x86 は非アラインを許す」の 2 つに同時に寄りかかる書き方で、
ARM では落ち、BE では値が化ける。クロスコンパイラが無い環境では後段だけ SKIP する。

`audit_cast_align.sh` と `check_le_access.py` は目的が違う — 前者は**ホストの gcc で
リポジトリ全域の候補を列挙する監査** (`make check` の外、人が仕分ける)、後者は**実ビルドの
コンパイラで対象 9 ファイルだけを検査する番人** (`make check` の中、落ちたら直す)。
対象の選び方と経緯は [tasks/portability/ARM_GAUGE.md](tasks/portability/ARM_GAUGE.md) §9 と
[tasks/portability/SURVEY_N1.md](tasks/portability/SURVEY_N1.md) (a)。

#### `tools/check_docs_links.py`
文書のリンク切れ検査 (`make check` の `check-docs-links`)。`lychee` (Rust 製のリンク検査器) を
`--offline --include-fragments` で呼ぶ薄い包みで、**相対パスの実在**と**見出しアンカーの実在**の
両方を見る。日本語の見出し (`08_build.md#配備3経路`) も GitHub と同じ規則で判定できる。
対象は `docs/**/*.md` (gitignore されたミラーの `docs/hw/` だけ除く) と `CLAUDE.md` /
`README.md` / `arch/README.md` / `platform/README.md` / `tools/tests/*_tdd.md`。
**`docs/archive/` は除外しない** — 完了した票を archive へ移したあとも相対パスが生きていることを
見るのが目的の 1 つで、移動で 1 段ずれるのはいちばん起きやすい壊し方だから。外部 URL は
見ない (`--offline`)。900 リンクで 0.03 秒。`lychee` が無い環境では `SKIP` と出して終了コード 0
(`cargo install lychee` で `~/.cargo/bin` に入り、包みが PATH に無くても探す)。

#### `tools/check_docs_orphans.py`
孤児文書の検出 — リンク切れの裏返しで、「**どこからも指されていない**」文書を挙げる
(lychee の守備範囲外なので自前、Python 標準ライブラリのみ)。`docs/INDEX.md` を唯一の起点として
相対リンクを推移的に辿り、到達できない `docs/**/*.md` を列挙する。`tools/tests/*_tdd.md` は票の
根拠なので索引から辿れる必要はなく、起点集合に `docs/tasks/**` と `docs/archive/**` の票を含め
(受入完了して archive へ落ちた票も票)、票が慣例どおり素のパスで書いた言及も参照とみなす。
索引に載せないと決めた例外は `docs/.orphans-allow` (1 行 1 パス、`#` コメント可) に理由つきで書く。

```bash
make check-docs-orphans        # 単体
```
2026-09-15 の棚卸し時点では docs 31 本 + TDD 記録 20 本が未参照だった (検査の不備ではなく、
票を書いて `INDEX.md` に載せ忘れた取りこぼしの実数)。そのあいだは門にすると通すために例外表へ
全部書き写すことになり `.orphans-allow` が「黙らせる表」に化けるので単体運用にしていたが、
索引を直して 0 になったので `make check` の列へ入れてある。

#### `tools/move_docs.py`

**文書を動かし、リポジトリ中の `.md` の参照を追従させる** (検査ではなく、手で回す道具)。
受入完了した票を `docs/archive/<領域>/` へ落とすときに使う。`git mv` だけでは指していた側の
相対リンクが黙って壊れ、`check-docs-links` が次に回るまで気づけない。

```bash
python3 tools/move_docs.py --into docs/archive/network docs/tasks/network/TASK_N0.md --dry-run
python3 tools/move_docs.py --map moves.tsv          # 1 行 "移動元<TAB>移動先"
python3 tools/move_docs.py SRC DST [SRC DST ...]
```

動かす一覧は道具の中に持たず、引数か TSV で外から与える。書き換えるのは 3 つの形だけで本文には
触らない — (1) `](相対パス#見出し)` と参照定義、(2) 地の文のルート相対パス言及 (票と
`tools/tests/*_tdd.md` が互いを指す慣例の書き方)、(3) 表示文字がパスそのもののリンクのラベル。
動いた文書自身の中のリンクは深さが変わるので全部引き直す。`--dry-run` で一覧だけ出せる。
運用 (何を落として何を残すか、落としたあとに守ること) は
[archive/README.md](archive/README.md)。実行後は `check-docs-links` / `check-docs-orphans` /
`gen_tests_inventory.py --write` の 3 つを回す。

**`--ext` / `--rewrite-only`** — 追従させるのは `.md` だけではない。票の番号や設計の正典は
ヘッダやモジュールの先頭コメントに「仕様: `docs/tasks/…`」と書く慣例があり、文書だけ動かすと
そこが古いまま残る (2026-09-16 のアーカイブで 27 ファイル取り残した)。

```bash
python3 tools/move_docs.py --map moves.tsv --rewrite-only \
    --ext .c,.h,.inc,.asm,.py,.rs,.mk,.toml,.yaml,.json,.sh,.1 \
    --exclude docs/hw,lib/sqlite3,lib/microtar,lib/zlib --dry-run
```

`--ext` は書き換え対象の拡張子 (既定 `.md`)。並べた拡張子**だけ**が対象になるので、`.md` も
一緒に書き換えるなら明示して並べる。`--exclude` はリポジトリ相対の接頭辞で、取り込んだ第三者の
ソースを外す。`--rewrite-only` は `git mv` を行わず**参照の書き換えだけ**を行う (移動が済んだ
後に取りこぼした種類のファイルを追従させるとき。移動元が無く移動先があることを確かめてから
当てる)。`.md` 以外では (2) 素のパス言及だけを当て、Markdown のリンク規則 (1)(3) は当てない
(C の `tbl[i](x)` に化けて当たる余地を残さないため)。**文字列リテラルの中も区別せずに当たる**
ので、ヘルプ文に文書パスを埋めている場合は `--dry-run` の一覧で確かめてから走らせる。
コメントだけの書き換えなら `.o` は変わらないはずで、疑わしければ `git archive HEAD` で展開した
無垢な木と同じフラグでコンパイルして md5 を突き合わせる。

### §8-5 開発環境の構築 (クロスコンパイラ)

OS32 の外部プログラムをビルドするためには、標準Cライブラリ (`newlib` - `libc.a`) と GCCライブラリ (`libgcc.a`) を含んだ `i386-elf` クロスコンパイラ環境が必要です。

> ⚠️ **コマンド名は `i386-elf-*` 固定**: `build/config.mk` は `i386-elf-gcc` /
> `i386-elf-ld` / `i386-elf-objcopy` というコマンド名と
> `$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0` というパスをハードコードしている。
> apt の `gcc-i686-elf` は `i686-elf-*` という名前になるうえ newlib を含まないため、
> **ソースからの構築が必須** (バージョンも 13.2.0 固定)。

#### ソースからの構築 (必須手順)

下の 4 段をそのまま実行するスクリプトが `tools/ci/build_cross.sh` にある
(tarball の sha256 検証と、出来上がりが nano 構成であることの検査つき。
`--src-dir` に tarball があればダウンロードしない)。GitHub Actions も同じスクリプトで
ツールチェーンを作る (§8-6)。

```bash
tools/ci/build_cross.sh --prefix $HOME/opt/cross --src-dir $HOME/opt/src --jobs $(nproc)
```

事前に必要な apt パッケージ:
```bash
sudo apt install build-essential nasm libgmp-dev libmpfr-dev libmpc-dev \
  texinfo bison flex python3-lz4 python3-yaml genisoimage
```

binutils 2.41 / GCC 13.2.0 / newlib 4.4.0.20231231 を以下の構成でビルドする
(インストール先の例: `$HOME/opt/cross`):

```bash
# 1. binutils
../binutils-2.41/configure --target=i386-elf --prefix=$HOME/opt/cross \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc) && make install

# 2. GCC ステージ1 (libgcc まで)
../gcc-13.2.0/configure --target=i386-elf --prefix=$HOME/opt/cross \
    --disable-nls --enable-languages=c --without-headers
make -j$(nproc) all-gcc all-target-libgcc
make install-gcc install-target-libgcc

# 3. newlib — ★nano構成必須★
../newlib-4.4.0.20231231/configure --target=i386-elf --prefix=$HOME/opt/cross \
    --disable-multilib \
    --disable-newlib-supplied-syscalls \
    --enable-newlib-nano-malloc \
    --enable-newlib-nano-formatted-io
make -j$(nproc) && make install

# 4. GCC 完全ビルド
../gcc-13.2.0/configure --target=i386-elf --prefix=$HOME/opt/cross \
    --disable-nls --enable-languages=c --with-newlib
make -j$(nproc) && make install
```

> ⚠️ **newlib の nano オプションを省略しないこと**: 通常構成の newlib では printf
> 系がフル実装になり、各コマンド .bin が約3倍 (14KB→40KB) に肥大化して
> 1.2MB ブートFDが容量不足でビルド失敗する (2026-08 環境再構築時に実証済み)。

#### Rust ツールチェーン

`userland/rust/` の Rust プログラム (hello_gfx, alloc_demo, math_test_rs) のビルドには
rustup が必要。バージョンは `rust-toolchain.toml` (nightly + rust-src) が自動解決する。

```bash
curl https://sh.rustup.rs -sSf | sh -s -- -y
```
カスタムターゲット `i686-os32-none.json` と build-std
(`userland/rust/.cargo/config.toml`) により `core`/`alloc`/`compiler_builtins` を
ソースからビルドする。

#### Makefile へのパス設定 (.env)
環境が構築できたら、OS32のソースツリー最上位の `.env` ファイルに以下を設定してください
(`Makefile` が `-include .env` で自動読み込み):

```env
# クロスコンパイラのインストール先
CROSS_DIR=/home/user/opt/cross
# NP21/W 本体ディレクトリ (np21x64w.exe, os32.nhd の場所)
NP21W_DIR=/mnt/c/Users/<user>/Documents/np21w
# HostDrv デプロイ先 (NP21/W の HOSTDRV0 設定と一致させる)
HOSTDRV_DIR=/mnt/c/os32
```
OS32の `Makefile` は、ここで指定された `$CROSS_DIR/i386-elf/include` や `$CROSS_DIR/i386-elf/lib` を参照してビルドを行います。
なお `tools/np21w_restart.py` は環境変数 `WIN_NP21W_DIR` (Windowsパス形式,
例 `C:\Users\<user>\Documents\np21w`) を参照する。

> [!NOTE]
> **コンパイラのバージョンについて**
> PC-98ターゲットでは新しいコンパイラの最適化やABI変更による非互換リスク（およびバグ）のほうが大きいため、一度安定動作したGCCバージョンで**完全に固定化**して開発を継続するのがセオリーです。OS32では当面GCC 13.x系の利用を推奨しています。

### §8-6 GitHub Actions での本体ビルド

`.github/workflows/build.yml` (workflow 名 `build`) が、素の clone から本体を完全ビルドして
成果物を artifact に置く。静的ゲートの `check.yml` とは別で、`make check` (試験) は回さない。

**狙い**: 開発ホストの回線が従量課金のことがあるので、ホストからはソースを push する
(小さい) だけにし、イメージの受け渡しは GitHub → 実機に繋がったホスト (Ubuntu ノート)
で済ませる。

| 項目 | 内容 |
|---|---|
| 起動 | `main` / `feat/**` への push、tag `v*` の push、手動 (workflow_dispatch)。同じ ref の古い run は打ち切る |
| ビルド | `make -j$(nproc) all [external] fd144` → `make deploy HOSTDRV_DIR=$RUNNER_TEMP/hostdrv NO_PRUNE=1`。`NP21W_DIR` は存在しない場所で、コピー失敗は Warning で続行する |
| submodule | **`apps/` と `game/` は private repo** なので既定の `GITHUB_TOKEN` では clone できない (初回 run はここで落ちた、2026-09-23)。リポジトリの secret **`SUBMODULE_TOKEN`** (os32-apps / os32-game の Contents: read を持つ fine-grained PAT) があれば取って `make external` まで回し、**無ければ warning を出して core だけ作る** (FD・ISO・packages・配備ツリーの本体側は揃う。apps/game の .bin だけ配備ツリーに入らない)。2 つの repo を public にすれば secret は要らない。**ユーザー決定 (2026-09-23): apps/game は含めない (core だけ)** |
| 文書だけの push | `docs/**`・`*.md`・`.claude/**` だけの push では回さない (`paths-ignore`)。同じ ref の run は 1 つ (`cancel-in-progress`) なので、**run の途中でその ref へ push すると打ち切られる** |
| ツールチェーン | `tools/ci/build_cross.sh` (§8-5) で `~/opt/cross` に作り、`actions/cache` で保存。キーは `cross-i386-elf-<OS>-<build_cross.sh のハッシュ>` なので、**スクリプトを変えたときだけ作り直す**。初回 (とキャッシュが消えたとき) は約 +25 分 (**実測 2026-09-23: ツールチェーン 24.9 分、run 全体 27 分**、ubuntu-latest 4 vCPU)。**キャッシュが効く 2 回目以降は run 全体で約 2 分** (実測 1 分 50 秒、キャッシュ 335MB) |
| Rust | `rust-toolchain.toml` を `rustup toolchain install` (引数なし) で解決。`Swatinem/rust-cache` で `target/` を保存 |
| 上限 | `timeout-minutes: 150` |

**成果物** (artifact 名 `os32-<ブランチ名の / を - に>-<sha7>`、保持 30 日。初回の実物 `os32-feat-gui-527255b` は zip で 13.8MB):

| ファイル | 中身 |
|---|---|
| `os32_boot.d88` / `os32_boot.img` | 2HD 1232KB の起動 FD (D88 / 生イメージ) |
| `os32_boot144.img` | 1.44MB の起動 FD (生イメージ、[POLICY_DEBUG.md §4-47](POLICY_DEBUG.md)) |
| `os32_install.iso` | インストール ISO |
| `packages/*.PKG` | パッケージ |
| `vmkernel.lz4` / `kernel.map` | カーネルとシンボル (kselftest の番地はこの map で引く) |
| `hostdrv.tar.gz` | `make deploy` の配備ツリー (HostDrv の `C:\os32` に相当) |
| `BUILD_INFO.txt` / `SHA256SUMS` | コミット・ref・日時・ランナー・gcc / rustc の版・submodule・各ファイルのサイズと sha256 |

**実機ホスト側の取り方** — `tools/ci_fetch.sh` (要 `gh`、`gh auth login` を 1 回。
public repo でも artifact の API ダウンロードには認証が要る):

```bash
tools/ci_fetch.sh                       # feat/gui の最新の成功 run
tools/ci_fetch.sh --branch main
tools/ci_fetch.sh --sha 0a5247f         # そのコミットの run (短縮 SHA 可)
tools/ci_fetch.sh --dry-run             # 選ばれる run と artifact を表示するだけ
```

保存先は `./os32-ci/<artifact 名>/` (`--dir` で変更)。取得後に `BUILD_INFO.txt` を表示し、
`SHA256SUMS` で全ファイルを照合する (不一致は終了 1、`gh` が無い / 未認証は終了 2)。

**tag → Release**: tag `v*` を push したときだけ、別 job (`contents: write` はその job だけ)
が同じファイルを GitHub Release に載せる。Release の asset は認証なしで取れる:

```bash
curl -fLO https://github.com/ske-studio/os32/releases/download/<tag>/os32_boot.d88
```

> ⚠️ artifact は「ビルドが通った」ことしか保証しない。NP21/W でも実機でも起動していない
> ([V4])。実機へ入れたら kselftest の値を**その artifact の `kernel.map`** の番地で読む。
