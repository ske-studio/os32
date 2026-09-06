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
| GUI シェル | [tasks/gui/TASKS.md](tasks/gui/TASKS.md) (§7 = 経過の正典) → 各票、[API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md) (凍結) | 票の排他ゾーン (§3) | `os32gui` → `/api/key` / `/api/mouse` (`ax/ay`) / `/api/screenshot`、`ring3_guard cirrus\|pegc\|bb`、`gfxmode` + reset で 3 バックエンド、Cirrus は ini の WAB ([D2]) |
| CI / 静的ゲート | `.github/workflows/check.yml` (ツールチェーン不要の検査だけ) | `tools/check_*.py` | push で自動。本体ビルドと実機は WSL 側 |
| V86 / DOS | [tasks/v86v2/README.md](tasks/v86v2/README.md) | `kernel/v86*.c` | `v86 -t` / `-b`、脱出は CTRL+STOP |
| LAN (LGY-98) | ドライバ [tasks/network/PLAN.md](tasks/network/PLAN.md) (M0〜M5、進捗 §9)、リンク層+Host Services [tasks/network/LINK_PLAN.md](tasks/network/LINK_PLAN.md) | `drivers/lgy98.c` `ne2000.c` `ne2000_ring.c` `ne2000_io.asm`、有効化は `make kernel-lgy98` (戻すのは `kernel-nolgy98`) | `make check` のホスト試験 (リング計算)、`make check-net-m2` (NP21/W で inject → 反射 → capture)、ini は [D2] |
| ビルド・配備の仕組みを変える | [08_build.md](08_build.md) | `Makefile` `build/*.mk` `tools/*.py` | `make check`、`os32-cycle deploy` |
| 障害を追う | [POLICY_DEBUG.md](POLICY_DEBUG.md) (§2 反映確認 → §4 教訓集 → §5 道具) | — | `tools/np21w_mcp/`、`/api/tvram` |
| 資料を直す | [INDEX.md](INDEX.md) の正典表で更新先を 1 つに決める | その 1 か所 + 参照側は要約のみ | `make check` (件数・制約 ID)、`make docs-win` |

## 2. ファイル地図 (どのファイルが何で、仕様はどこか)

### カーネル `kernel/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `kernel.c` `kentry.asm` | 起動、シェル起動ループ (CUI `/sys/shell.bin` ⇄ GUI `/bin/gshell.bin` を `gui_take_next_shell()` で往復)、`/etc/system.cfg` の反映 | [01 §1-2](01_system.md) |
| `sysconfig.c` | `/etc/system.cfg` の解析 (`GUI=0/1`、`GFX=pc98\|pegc\|cirrus\|auto`) | [01 §1-2](01_system.md)、[tasks/gui/TASK_K4](tasks/gui/TASK_K4_gui_boot.md) |
| `gui.c` | GUI の背骨: `gui_call` (アプリ → WM の唯一の入口)、`gui_register`、所有者回収 `gui_owner_exit`、次シェル要求 | [tasks/gui/API_CONTRACTS.md T1〜T9](tasks/gui/API_CONTRACTS.md) |
| `shlib.c` | 共有ライブラリ帯 0x400000〜 のロード (`OS32ShlibHeader`) と、アプリ PD ごとの .data/.bss 複製 (`shlib_addrspace_attach`) | [02 §2-1](02_memory.md)、[09](09_exec.md)、[tasks/gui/TASK_K3](tasks/gui/TASK_K3_shared_lib_band.md) |
| `paging.c` `pgalloc.c` | ページテーブル (守備範囲 32MB、実 RAM 管理は 16MB)、ガードページ、物理ページ確保。PD はプログラムごと (v2 M1)。デバイス窓は `paging_map_phys` (supervisor+PCD)、クライアント面の USER 昇格は `paging_addrspace_map_user_keep` | [02](02_memory.md)、[tasks/v2/M1_RING3.md](tasks/v2/M1_RING3.md) |
| `kmalloc.c` | カーネルヒープ (320KB) | [02 §2-1](02_memory.md) |
| `shm.c` | 共有メモリ 16 ブロック × 16KB。ブロック 0 = DB 結果、12〜15 = GUI (K1 票) | [02](02_memory.md)、[tasks/gui/API_CONTRACTS.md T2](tasks/gui/API_CONTRACTS.md) |
| `idt.c` `isr_*.c` | 割り込み、`int 0x80` KAPI トランポリン着地点 | [04](04_interrupts.md)、[tasks/v2/M2](tasks/v2/M2_KAPI_TRAMPOLINE.md) |
| `ime.c` `ime_romkana.c` `ime_dict.c` `ime_render*.c` | FEP。描画は関数表 (`ime_render.h`) 越し。GUI は `ime_feed_key` / `ime_set_render` (KAPI v42) で WM が FEP を持つ | [tasks/fep/](tasks/fep/00_INDEX.md)、[tasks/gui/TASK_W2](tasks/gui/TASK_W2_fep_lease_modal.md) |
| `ring3_entry.asm` | `int 0x80` の入口 (`int80_stub`: セグメント復元後 `sti`、出口 `cli`)、`kapi_invoke` | [09](09_exec.md)、[POLICY_DEBUG §4-19](POLICY_DEBUG.md) |
| `console.c` | TVRAM 出力、スクロール予約 (`tvram_set_scroll_reserve`)、GDC カーソル | [05 §5-9](05_drivers.md) 周辺、[POLICY_DEBUG §4-18](POLICY_DEBUG.md) |
| `snd_engine.c` | FM/SSG シーケンサ (タイマ IRQ 駆動) | [05 §5-3](05_drivers.md) |
| `v86*.c` | V86 モニタ、仮想 PIC、キー所有権、脱出キー | [tasks/v86v2/](tasks/v86v2/README.md) |
| `kselftest.c` | ブート時セルフテスト (kstring / kmalloc / kprintf)。プリミティブを触ったら項目を足す | [POLICY_DEBUG §2](POLICY_DEBUG.md) |

### 実行と KernelAPI `exec/` `kapi/` `sdk/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `exec/exec.c` `exec_heap.c` | OS32X ローダ (ヘッダ v2 `load_addr`)、子プロセス帯の動的レイアウト、Ring3 AS の USER 写像 (プログラム帯 / スタック / VRAM / SHM / フォント / バックバッファ / shlib)、`exec_launch_abort` (起動失敗の巻き戻し)、所有者別の資源回収、CTRL+STOP (`ring3_abort_*`)、syscall 境界ポンプ (`ring3_gui_pump`)、`exec_run` の setjmp 分割壁 | [09](09_exec.md) |
| `exec/exec_kapi_init.inc` | KAPI 表の初期化 (生成物) | [KAPI_SPEC §3-1](KAPI_SPEC.md) |
| `kapi/kapi_generated.c` `kapi_sys.c` `kapi_db.c` | `__cdecl` ラッパー (生成 + 手書き) | 同上 |
| `sdk/kapi.json` | **KAPI の正典** | 同上 |
| `sdk/include/os32/os32_kapi_shared.h` | 構造体・`KAPI_VERSION`・`OS32_ERR_*` (エラーコードの正典)・OS32X ヘッダ v2・`OS32ShlibHeader` | 同上 |
| `sdk/include/os32/os32_kapi_slots.h` | `KAPI_SLOT_*` = `int 0x80` のスロット番号 (生成物、型非依存) | [KAPI_SPEC §3-1](KAPI_SPEC.md) |
| `sdk/include/os32/os32_gui_shared.h` `sdk/rust/os32api/src/gui/` | GUI の op / イベント / SHM レイアウト (C と Rust に同じ値) | [tasks/gui/API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md)、[tasks/gui/PROTO_LAYOUT.md](tasks/gui/PROTO_LAYOUT.md) |
| `sdk/link/app.ld` `app_sys.ld` `shlib.ld` `sdk/crt/crt0.asm` | アプリ (0x500000) / シェル帯 (0x300000、gshell) / 共有ライブラリ (0x400000) のリンク | [09](09_exec.md)、[02 §2-1](02_memory.md) |
| `sdk/gen_kapi.py` `kapi_rust_gen.py` `mkos32x.py` `tools/mkshlib.py` | KAPI 生成、OS32X ヘッダ付与、`.shlib` 生成と番号表照合 (`make check-shlib`) | [08 §8-4](08_build.md) |

### ドライバ `drivers/`

| ファイル | 役割 | 仕様 |
|---|---|---|
| `fdc.c` `disk.c` `dev.c` `loop_dev.c` | FDC、論理ディスク、デバイス登録簿、ループバック | [05 §5-2, §5-11](05_drivers.md) |
| `ide.c` `atapi.c` | IDE HDD、ATAPI CD-ROM | [06 §6-3](06_filesystem.md)、[05 §5-6](05_drivers.md) |
| `kbd.c` | PC-98 キーボード、修飾キー、V86 への注入。GUI 中は cooked に積まず raw リング (`kbd_trygetrawkey`、`keycode\|down<<8\|mods<<9`)、CTRL+STOP → `ring3_abort_request` | [05 §5-1](05_drivers.md) |
| `serial.c` | RS-232C (rshell / ai-debug の経路) | [05 §5-4](05_drivers.md) |
| `mouse*.c` | バスマウス + NP21/W シームレスマウス (座標は移動範囲へ比例配分、480 ライン可) | [05 §5-7](05_drivers.md) |
| `wab_glue.h` `wab_glue_xe10.c` `wab_cirrus.c` | ウィンドウアクセラレータ: ボードグルー契約 / Xe10 内蔵 (ID 5Bh、0FAAh/0FABh) / CL-GD5430 チップ (BLT は I/O 経由、8bpp、DAC)。定数は `include/wab_xe10.h` | [tasks/gui/DESIGN.md §6〜§8](tasks/gui/DESIGN.md)、[tasks/gui/TASK_H3](tasks/gui/TASK_H3_cirrus.md) |
| `fm.c` | OPN/OPM | [05 §5-3](05_drivers.md) |
| `kcg.c` | 漢字 ROM / ビットマップフォント | [05 §5-9](05_drivers.md) |
| `np2sysp.c` | NP21/W ハイパーコール | [05 §5-10](05_drivers.md) |
| `lgy98.c` `ne2000.c` `ne2000_ring.c` `ne2000_io.asm` `ne2000_regs.h` | LGY-98 (NE2000 互換 C バス LAN)。カード固有 / 8390 本体 / リング計算 (ホスト試験可) / 16bit PIO / レジスタ定数の正典。既定は無効 (`CONFIG_LGY98_BASE=0`) | [tasks/network/PLAN.md](tasks/network/PLAN.md) |

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
| `gfx/gfx_core.c` `gfx_vram.c` `palette.c` | HAL の入口: バックエンド選択 (probe 順 Cirrus → PEGC → 9801、`GFX=` の強制)、present (ページフリップ)、パレット、カウンタ (`gfx_stats`)、`gfx_bb_phys_range` | [05 §5-5](05_drivers.md) |
| `gfx/backend_pc98.c` `backend_pegc.c` `backend_cirrus.c` `include/gfx_hal.h` | バックエンド表 `GfxBackend` (probe / init / query / present_rect / fill / blit / enter / leave / shutdown) と 3 実装: 9801 4 プレーン (主記憶 BB 0x6A000)、PEGC 640×480×256 (F00000h 窓、BB は物理末尾 300KB)、Cirrus GD5430 (01000000h リニア窓、表示面 + クライアント面、エンジン BLT) | [05 §5-5](05_drivers.md)、[02 §2-1](02_memory.md)、[tasks/gui/DESIGN.md](tasks/gui/DESIGN.md) |
| `lib/sqlite3/os32_sqlite_vfs.c` `os32_sqlite_config.h` `sqlite_stack.asm` | カーネル内 SQLite (0x200000 帯、MEMSYS5 384KB、代替スタック) | [tasks/sqlite/](tasks/sqlite/00_INDEX.md) |
| `boot/boot_*.asm` `loader_*.asm` `boot_main.c` `ext2_mini.c` `lz4_mini.c` | IPL、第 2 段ローダ (PM 遷移を含む、分割禁止)、LZ4 展開 | [01 §1-2](01_system.md)、[10 §10-2, §10-3](10_notes.md)、[tasks/boot_reform/](tasks/boot_reform/00_OVERVIEW.md) |

### ユーザ空間 `userland/`

| 場所 | 役割 | 仕様 |
|---|---|---|
| `shell/main.c` `ui.c` `cmd_*.c` `cmd_script.c` `rshell.c` | 常駐 CUI シェル (0x300000, CPL=0)。コマンド登録 (`ShellCmd`, 最大 128)、行編集・補完・履歴、スクリプト、シリアル rshell、`os32gui` / `gfxmode` | [07](07_shell.md) |
| `gshell/` (Rust) | GUI シェル = WM (シェル帯 0x300000 に CUI と入れ替わりで常駐、`/bin/gshell.bin`)。`wm.rs` 窓 / Z 順 / 所有者、`handler.rs` op 表、`input.rs` X3/X4 の入力取り込み (raw キー・FEP 退避・ボタンエッジの領分)、`visible.rs` 可視領域、`fep.rs` (カーネル FEP を `ime_feed_key` で駆動)、`lease.rs` `modal.rs` `timer.rs` `chrome.rs` `cursor.rs` | [tasks/gui/TASK_W1](tasks/gui/TASK_W1_wm_core.md)、[TASK_W2](tasks/gui/TASK_W2_fep_lease_modal.md)、[API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md) |
| `rust/libos32gui/` `libos32gui_stub/` | GUI クライアント (G 描画 / `gui_call` / U3 ループ / ウィジェット木 / 箱レイアウト)。`.shlib` として 0x400000 に常駐、アプリは stub (ジャンプ表) をリンク | [tasks/gui/TASK_C1〜C3](tasks/gui/TASK_C3_shared_lib.md) |
| `cmds/` `system/` `tests/` `rust/` | コマンド、システムユーティリティ、テスト (`hal_test` `gdi_test` `ring3_guard` `gui_busy` `lease_test` `gui_bench` …)、Rust (Cargo ワークスペース、`os32api` クレート) | 一覧は各 `deploy.yaml` と [07 §7-1](07_shell.md) |
| `lib/os32` `math` `input` `gfx` `snd` `db` | 基盤ライブラリ (デバッグ出力 / 整数数学 / 入力抽象 / 描画 (4 プレーンと PACKED8 の両経路、`libos32gfx_attach`) / FM・SSG / SQLite ラッパ) | `tasks/lib*/`、[05 §5-5](05_drivers.md) |
| `lib/tilemap` `ui` `filer` `md` `asset` `ecs` `save` | 描画・UI 系 (タイルマップ / microUI (テスト導入) / ファイラ / Markdown / アセット / ECS / セーブ) | 同上 |
| `apps/` `game/` (submodule) | 標準アプリ、対戦スゴロク RPG。ゲームエンジン 11 ライブラリは os32-game が自前ビルド | 各リポジトリの `docs/` |

## 3. 参照

- Claude Code / Codex 共用スキル: [os32-emu-debug](../.claude/skills/os32-emu-debug/SKILL.md) (NP21/W 調査)、[os32-build-verify](../.claude/skills/os32-build-verify/SKILL.md) (変更別ビルド・反映確認)。本文は `.claude/skills/`、Codex の `.agents/skills/` から同じフォルダを参照する。`.agents/` は現行の Git 除外対象なので共有リンクは環境ごとに設置する (例: `.agents/skills/os32-emu-debug` → `../../.claude/skills/os32-emu-debug`)。
- 規則の正典: [CONSTRAINTS.md](CONSTRAINTS.md) (CLAUDE.md / SOUL.md は ID で参照)
- 成果の履歴: [CHANGELOG.md](../CHANGELOG.md)、[archive/ROADMAP_v1.0.md](archive/ROADMAP_v1.0.md)。計画: [ROADMAP.md](ROADMAP.md)
- 落とし穴の経緯: [POLICY_DEBUG.md §4](POLICY_DEBUG.md)。短い注意は CLAUDE.md「Known Gotchas」
