# OS32 開発案内 — 作業別の参照先とファイル地図

> 役割 (2026-09-05 に再定義): **「何をしたいか」から「どこを読み、どこを触り、どう検証するか」へ
> 案内する**文書。仕様・規則・番地・手順の本文はここに置かない (正典は
> [INDEX.md「情報単位ごとの正典」](INDEX.md) の表)。ここに書くのは参照先と、
> ファイルと役割の対応だけ。

---

## 1. 作業別の参照先

| やりたいこと | 先に読む | 触る場所 | 検証 |
|---|---|---|---|
| KernelAPI を足す | [KAPI_SPEC.md §3-1](KAPI_SPEC.md) (手順の正典) | `sdk/kapi.json` → 再生成 → 実体 | `make clean && make all`、`make check`、NHD 配備後に呼ぶテスト |
| カーネルを直す | 該当部の仕様 [01](01_system.md)〜[10](10_notes.md)、[CONSTRAINTS.md](CONSTRAINTS.md) | `kernel/` `drivers/` `fs/` `gfx/` (下の地図) | `make kernel` → NP21/W 停止 → `make deploy-kernel` → 起動 → kselftest と `ver` ([POLICY_DEBUG §2](POLICY_DEBUG.md)) |
| ドライバを足す / 変える | [05_drivers.md](05_drivers.md)、ハード資料 `docs/hw/` (UNDOCUMENTED 優先) | `drivers/`、定数は [C4] の三層 | 同上。GUI 向けバックエンドは [tasks/gui/DESIGN.md §7](tasks/gui/DESIGN.md) の指針 |
| 外部プログラムを書く | [09_exec.md](09_exec.md)、[KAPI_SPEC.md](KAPI_SPEC.md)、`sdk/example/` | `userland/` (本体) / `apps/` `game/` (submodule) | 自層の `deploy.yaml` に登録 ([V2])、`make hotdeploy` で回し、最後は NHD 配備 ([V1]) |
| ユーザ空間ライブラリを触る | 各設計書 `tasks/lib*/`、[07_shell.md](07_shell.md) | `userland/lib/<name>/` | 対応する `userland/tests/<name>_test` |
| シェルを触る | [07_shell.md](07_shell.md) | `userland/shell/` | rshell から `/api/cmd`。パイプ・リダイレクトは `ext_cmd1 \| ext_cmd2` も |
| 日本語入力 (FEP) | [tasks/fep/00_INDEX.md](tasks/fep/00_INDEX.md) (進捗の正典) | `kernel/ime*.c` | メモリ os32-fep-testing の手順、`/api/key` |
| GUI シェル | [tasks/gui/TASKS.md](tasks/gui/TASKS.md) → 各票 | 票の排他ゾーン | 票のゲート |
| V86 / DOS | [tasks/v86v2/README.md](tasks/v86v2/README.md) | `kernel/v86*.c` | `v86 -t` / `-b`、脱出は CTRL+STOP |
| LAN (LGY-98) | [tasks/network/PLAN.md](tasks/network/PLAN.md) (M0〜M5) | `drivers/lgy98.c` `ne2000*.{c,asm}` (予定) | NP21/W の `/api/net/*` と `np2net_helper.py` (計画 §8)、ini 追加は [D2] |
| ビルド・配備の仕組みを変える | [08_build.md](08_build.md) | `Makefile` `build/*.mk` `tools/*.py` | `make check`、`os32-cycle deploy` |
| 障害を追う | [POLICY_DEBUG.md](POLICY_DEBUG.md) (§2 反映確認 → §4 教訓集 → §5 道具) | — | `tools/np21w_mcp/`、`/api/tvram` |
| 資料を直す | [INDEX.md](INDEX.md) の正典表で更新先を 1 つに決める | その 1 か所 + 参照側は要約のみ | `make check` (件数・制約 ID)、`make docs-win` |

## 2. ファイル地図 (どのファイルが何で、仕様はどこか)

### カーネル `kernel/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `kernel.c` `kentry.asm` | 起動、シェル起動ループ、`/etc/system.cfg` (GUI 化は K4 票) | [01 §1-2](01_system.md) |
| `paging.c` `pgalloc.c` | ページテーブル、ガードページ、物理ページ確保。PD はプログラムごと (v2 M1) | [02](02_memory.md)、[tasks/v2/M1_RING3.md](tasks/v2/M1_RING3.md) |
| `kmalloc.c` | カーネルヒープ (320KB) | [02 §2-1](02_memory.md) |
| `shm.c` | 共有メモリ 16 ブロック × 16KB。ブロック 0 = DB 結果、12〜15 = GUI (K1 票) | [02](02_memory.md)、[tasks/gui/API_CONTRACTS.md T2](tasks/gui/API_CONTRACTS.md) |
| `idt.c` `isr_*.c` | 割り込み、`int 0x80` KAPI トランポリン着地点 | [04](04_interrupts.md)、[tasks/v2/M2](tasks/v2/M2_KAPI_TRAMPOLINE.md) |
| `ime.c` `ime_romkana.c` `ime_dict.c` `ime_render*.c` | FEP。描画は関数表 (`ime_render.h`) 越し | [tasks/fep/](tasks/fep/00_INDEX.md) |
| `console.c` | TVRAM 出力、スクロール予約 (`tvram_set_scroll_reserve`)、GDC カーソル | [05 §5-9](05_drivers.md) 周辺、[POLICY_DEBUG §4-18](POLICY_DEBUG.md) |
| `snd_engine.c` | FM/SSG シーケンサ (タイマ IRQ 駆動) | [05 §5-3](05_drivers.md) |
| `v86*.c` | V86 モニタ、仮想 PIC、キー所有権、脱出キー | [tasks/v86v2/](tasks/v86v2/README.md) |
| `kselftest.c` | ブート時セルフテスト (kstring / kmalloc / kprintf)。プリミティブを触ったら項目を足す | [POLICY_DEBUG §2](POLICY_DEBUG.md) |

### 実行と KernelAPI `exec/` `kapi/` `sdk/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `exec/exec.c` `exec_heap.c` | OS32X ローダ、子プロセス帯の動的レイアウト、所有者別の資源回収、`exec_run` の setjmp 分割壁 | [09](09_exec.md) |
| `exec/exec_kapi_init.inc` | KAPI 表の初期化 (生成物) | [KAPI_SPEC §3-1](KAPI_SPEC.md) |
| `kapi/kapi_generated.c` `kapi_sys.c` `kapi_db.c` | `__cdecl` ラッパー (生成 + 手書き) | 同上 |
| `sdk/kapi.json` | **KAPI の正典** | 同上 |
| `sdk/include/os32/os32_kapi_shared.h` | 構造体・`KAPI_VERSION`・`OS32_ERR_*` (エラーコードの正典) | 同上 |
| `sdk/link/app.ld` `sdk/crt/crt0.asm` | アプリのリンク (0x400000。共有ライブラリ導入時に 0x500000 へ: K3 票) | [09](09_exec.md) |

### ドライバ `drivers/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `fdc.c` `disk.c` `dev.c` `loop_dev.c` | FDC、論理ディスク、デバイス登録簿、ループバック | [05 §5-2, §5-11](05_drivers.md) |
| `ide.c` `atapi.c` | IDE HDD、ATAPI CD-ROM | [06 §6-3](06_filesystem.md)、[05 §5-6](05_drivers.md) |
| `kbd.c` | PC-98 キーボード、修飾キー、V86 への注入 | [05 §5-1](05_drivers.md) |
| `serial.c` | RS-232C (rshell / ai-debug の経路) | [05 §5-4](05_drivers.md) |
| `mouse*.c` | バスマウス + NP21/W シームレスマウス | [05 §5-7](05_drivers.md) |
| `fm.c` | OPN/OPM | [05 §5-3](05_drivers.md) |
| `kcg.c` | 漢字 ROM / ビットマップフォント | [05 §5-9](05_drivers.md) |
| `np2sysp.c` | NP21/W ハイパーコール | [05 §5-10](05_drivers.md) |
| (予定) `lgy98.c` `ne2000.c` `ne2000_io.asm` | LGY-98 (NE2000 互換 C バス LAN)。未実装 | [tasks/network/PLAN.md](tasks/network/PLAN.md) |

### ファイルシステム `fs/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `vfs.c` `vfs_fd.c` | VFS、FD 表、所有者タグ・保護 FD | [06 §6-1](06_filesystem.md) |
| `ext2_*.c` | ext2 読み書き (NHD の主 FS) | [06 §6-2](06_filesystem.md) |
| `fatfs/` `fatfs_vfs.c` | FatFs (唯一の FAT 実装) | [06 §6-8](06_filesystem.md) |
| `hostdrvfs.c` | HostDrv (`/host`) | [06 §6-7](06_filesystem.md) |
| `pipe_buffer.c` `fd_redirect.c` | パイプ・リダイレクト | [06 §6-4, §6-5](06_filesystem.md) |
| `iso9660.c` | CD-ROM | [06 §6-6](06_filesystem.md) |

### グラフィックス `gfx/`、SQLite `lib/sqlite3/`、ブート `boot/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `gfx/gfx_core.c` `gfx_vram.c` `palette.c` | バックバッファ、present (ページフリップ)、パレット。GUI 向けにバックエンド表へ組み替える (H1 票) | [05 §5-5](05_drivers.md) |
| `lib/sqlite3/os32_sqlite_vfs.c` `os32_sqlite_config.h` `sqlite_stack.asm` | カーネル内 SQLite (0x200000 帯、MEMSYS5 384KB、代替スタック) | [tasks/sqlite/](tasks/sqlite/00_INDEX.md) |
| `boot/boot_*.asm` `loader_*.asm` `boot_main.c` `ext2_mini.c` `lz4_mini.c` | IPL、第 2 段ローダ (PM 遷移を含む、分割禁止)、LZ4 展開 | [01 §1-2](01_system.md)、[10 §10-2, §10-3](10_notes.md)、[tasks/boot_reform/](tasks/boot_reform/00_OVERVIEW.md) |

### ユーザ空間 `userland/`

| 場所 | 役割 | 仕様 |
|---|---|---|
| `shell/main.c` `ui.c` `cmd_*.c` `cmd_script.c` `rshell.c` | 常駐シェル (0x300000, CPL=0)。コマンド登録 (`ShellCmd`, 最大 128)、行編集・補完・履歴、スクリプト、シリアル rshell | [07](07_shell.md) |
| `cmds/` `system/` `tests/` `rust/` | コマンド、システムユーティリティ、テスト、Rust (Cargo ワークスペース、`os32api` クレート) | 一覧は各 `deploy.yaml` と [07 §7-1](07_shell.md) |
| `lib/os32` `math` `input` `gfx` `snd` `db` | 基盤ライブラリ (デバッグ出力 / 整数数学 / 入力抽象 / 描画 / FM・SSG / SQLite ラッパ) | `tasks/lib*/`、[05 §5-5](05_drivers.md) |
| `lib/tilemap` `ui` `filer` `md` `asset` `ecs` `save` | 描画・UI 系 (タイルマップ / microUI (テスト導入) / ファイラ / Markdown / アセット / ECS / セーブ) | 同上 |
| `apps/` `game/` (submodule) | 標準アプリ、対戦スゴロク RPG。ゲームエンジン 11 ライブラリは os32-game が自前ビルド | 各リポジトリの `docs/` |

## 3. 参照

- Claude Code / Codex 共用スキル: [os32-emu-debug](../.claude/skills/os32-emu-debug/SKILL.md) (NP21/W 調査)、[os32-build-verify](../.claude/skills/os32-build-verify/SKILL.md) (変更別ビルド・反映確認)。本文は `.claude/skills/`、Codex の `.agents/skills/` から同じフォルダを参照する。`.agents/` は現行の Git 除外対象なので共有リンクは環境ごとに設置する (例: `.agents/skills/os32-emu-debug` → `../../.claude/skills/os32-emu-debug`)。
- 規則の正典: [CONSTRAINTS.md](CONSTRAINTS.md) (CLAUDE.md / SOUL.md は ID で参照)
- 成果の履歴: [CHANGELOG.md](../CHANGELOG.md)、[archive/ROADMAP_v1.0.md](archive/ROADMAP_v1.0.md)。計画: [ROADMAP.md](ROADMAP.md)
- 落とし穴の経緯: [POLICY_DEBUG.md §4](POLICY_DEBUG.md)。短い注意は CLAUDE.md「Known Gotchas」
