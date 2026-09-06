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
`check-privileged` `check-ne2000-ring` `check-shlib`)。`emu_agent` (ローカル AI) の `make` は
許可リスト (`tools/emu_agent/agent.py` の `MAKE_TARGETS`) に載ったターゲットしか実行しない。

**GitHub Actions** (`.github/workflows/check.yml`、2026-09-06): push / PR で、クロスツールチェーン無しで
回せる検査だけを自動ゲートにする — KAPI 版番号の一致、`sdk/kapi.json` からの生成物がコミット済みと
一致すること ([ABI1])、CONSTRAINTS ⇄ CLAUDE.md、`mkshlib --check`、ne2000 リングのホストテスト。
`check-manifests` は `make all` の成果物を見るので対象外 (WSL 側の `make check` で回す)。

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
├── lib/            汎用ライブラリ (utf8, path, sqlite3 等)
├── include/        システム統合用共通ヘッダ群 (memmap.h, gfx_hal.h, wab_xe10.h 等)
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
├── tools/          ホスト上でのイメージ生成・デプロイ・検査ツール (nhd_deploy, hotdeploy, mkshlib, check_*, emu_agent/ (ローカル AI の実機操作), np21w_mcp/)
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
make hotdeploy FILE=apps/foo/foo.bin                        # ホットデプロイ (再起動不要)
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
| `make deploy` | HostDrv (`C:\os32`) への同期 — 再起動不要 |
| `make deploy-kernel` | HostDrv同期 + HostDrv→ext2同期 + NHDコピー — **要NP21/W再起動** |
| `make deploy-boot` | ブートローダー (loader_hdd.bin) をNHDブート領域へ書き込み |
| `make deploy-nhd` | deploy.yaml フルデプロイ + NHDコピー — **要NP21/W再起動** |
| `make prune-stale` / `make prune-stale-delete` | 配備先 (HostDrv + NHD) に残ったマニフェストに無い *.bin を一覧 / 削除。deploy 系は既定で削除まで行う (`NO_PRUNE=1` で一覧のみ) |
| `make apps` / `make game` | 外部リポジトリ (git submodule `apps/` = os32-apps、`game/` = os32-game) を SDK 経由でビルド。空なら `git submodule update --init` を促す |
| `make external` | 上記 2 つをまとめて。KAPI を動かしたあとは必ずこれで再ビルドし、submodule のポインタを更新してコミットする |
| `make clean-external` | 外部リポジトリの生成物を削除 |
| `make hotdeploy FILE=<path>` | 個別バイナリのホットデプロイ — 再起動不要。ユーザーランドのみ |
| `make nhd-pull` | Windows 側 NHD を /tmp に取り込む (フォーマットしない) |
| `make nhd-init` | 初回セットアップ — **フォーマットするのでゲスト側データが消える** |

ビルド側のターゲットは `make all` / `kernel` / `libs` / `programs` / `sdk` /
`apps` / `game` / `clean` / `clean-kernel` / `clean-libs` / `clean-programs`。
KernelAPI の構造体を変えたときは `make clean` → `make all` が必須
(古い `.o` が残ると ABI 不整合で静かに壊れる)。
| `make nhd-init` / `nhd-mount` / `nhd-umount` | 初期化・マウント操作 |

#### `tools/hostdrv_deploy.py`
HostDrv デプロイ先 (`HOSTDRV_DIR`, 既定 `C:\os32`) への差分同期。sudo 不要で高速。`make deploy` から呼ばれる。

#### `tools/mkpkg.py`
OS32パッケージ (.PKG) を生成するビルダー。複数ファイルをLZSS圧縮し、hash-chainで連結したパッケージを生成する。CDインストーラ (`cdinst.bin`) と連携し、ISOイメージ経由でのプログラム配布に使用される。

```bash
python3 tools/mkpkg.py --defs tools/package_defs.yaml --output packages/ --base .
```
- `make packages` ターゲットで自動実行 (定義: `tools/package_defs.yaml`)
- `make iso` で `genisoimage` を使用しISOイメージを生成

### §8-5 開発環境の構築 (クロスコンパイラ)

OS32 の外部プログラムをビルドするためには、標準Cライブラリ (`newlib` - `libc.a`) と GCCライブラリ (`libgcc.a`) を含んだ `i386-elf` クロスコンパイラ環境が必要です。

> ⚠️ **コマンド名は `i386-elf-*` 固定**: `build/config.mk` は `i386-elf-gcc` /
> `i386-elf-ld` / `i386-elf-objcopy` というコマンド名と
> `$(CROSS_DIR)/lib/gcc/i386-elf/13.2.0` というパスをハードコードしている。
> apt の `gcc-i686-elf` は `i686-elf-*` という名前になるうえ newlib を含まないため、
> **ソースからの構築が必須** (バージョンも 13.2.0 固定)。

#### ソースからの構築 (必須手順)

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
