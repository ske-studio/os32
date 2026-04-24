# タスク06: 実装フェーズ計画・TODO

## 進捗サマリ

| Phase | 内容 | 状態 |
|-------|------|------|
| Phase 0 | 事前準備 (amalgamation生成, OMIT検証, コンパイル確認) | ✅ 完了 |
| Phase 1 | カーネル統合基盤 (分離配置, VFS, メモリDB動作確認) | ✅ 完了 |
| Phase 2 | KAPI + IPC レイヤー (kapi_db, libos32db, db_test) | 🔲 次 |
| Phase 3 | 堅牢化 (exec_exit連携, fsync, エッジケース) | 🔲 未着手 |

---

## Phase 0: 事前準備 ✅ 完了

- [x] **amalgamation 生成**
  - sqlite-src-3530000 + tclsh で OMIT マクロ付き amalgamation を再生成
  - `sqlite3.c` + `sqlite3.h` を `lib/sqlite3/` に配置

- [x] **コンパイル設定の確定**
  - `os32_sqlite_config.h` に全マクロ集約 (32個のOMIT)
  - OMIT_CTE / OMIT_WINDOWFUNC: ソースから OMIT 付きで amalgamation を再生成
  - C89互換パッチ: 不要 (`-Wno-long-long` のみで対応)

- [x] **クロスコンパイル検証**
  - `i386-elf-gcc -std=gnu89 -Os` でエラー 0 確認
  - `.text` サイズ実測: **360KB** (`-Os`), 527KB (`-O2`)

- [x] **メモリ配置方針の確定**
  - `.text` 360KB → カーネル拡張域 `0x18A000-0x1FFFFF` (472KB) に収まる
  - コンベンショナルメモリ (220KB) には収まらないが、拡張メモリ域で解決

- [x] **ディレクトリ構成作成**

---

## Phase 1: カーネル統合基盤 ✅ 完了

### 1.1 分離配置アーキテクチャ ✅

`kernel.bin` と `sqlite.bin` にバイナリを物理分離。
カーネル起動時に `sqlite.bin` を VFS 経由で拡張メモリにロードする。

```
[kernel.bin]  94KB   → NHDブート領域 (LBA 6-)
[sqlite.bin] 373KB   → ext2 /sys/sqlite.bin → 0x18A000 にロード
```

**成果物:**
- `build/os32.ld` — EXCLUDE_FILE + KEEP で SQLite セクションを分離
- `Makefile` — `objcopy` で kernel.elf → kernel.bin + sqlite.bin を抽出

### 1.2 リンカスクリプト ✅

```
.text       0x9000   カーネルコード (EXCLUDE_FILE で SQLite除外)
.data       カーネルデータ
.bss        カーネルBSS (__bss_start ~ __bss_end, kentry でゼロクリア)

--- 0x18A000 --- SQLite 拡張域 ---

.sqlite_text     SQLite + VFS + テストのコード (KEEP)
.sqlite_rodata   SQLite の読み取り専用データ (KEEP)
.sqlite_data     SQLite のグローバル変数 (KEEP)
.sqlite_bss      MEMSYS5 プール 100KB (NOLOAD)
```

**セクション配置 (実測値):**

| セクション | 開始 | 終了 | サイズ |
|-----------|------|------|--------|
| `.sqlite_text` | `0x18A000` | `0x1DC04B` | 328KB |
| `.sqlite_rodata` | `0x1DC060` | `0x1E3E4B` | 32KB |
| `.sqlite_data` | `0x1E3E60` | `0x1E51D7` | 5KB |
| `.sqlite_bss` | `0x1E52E0` | `0x1FE55F` | 100KB |

### 1.3 VFS 実装 ✅

`lib/sqlite3/os32_sqlite_vfs.c`:
- `xOpen` / `xClose` / `xRead` / `xWrite` / `xFileSize`
- `xSync` (ext2_sync)
- `xDelete` / `xAccess` / `xFullPathname`
- `xTruncate` (no-op — ext2 truncate 未実装)
- `xRandomness` / `xSleep` / `xCurrentTime`
- `xLock` / `xUnlock` / `xCheckReservedLock` (no-op — シングルタスク)
- `sqlite3DbIsNamed` スタブ (OMIT_ATTACH 対応)
- MEMSYS5 固定プール 100KB (`sqlite_mem_pool[]`)

### 1.4 カーネル初期化統合 ✅

`kernel/kernel.c` の `kernel_main()` にて:

1. `vfs_read("/sys/sqlite.bin", 0x18A000, 472KB)` — バイナリロード
2. `.sqlite_bss` ゼロクリア (`__sqlite_data_end` ～ `__sqlite_end`)
3. `os32_sqlite_init()` — `sqlite3_config(MEMSYS5)` + `sqlite3_initialize()`
4. `os32_sqlite_test()` — メモリDB CRUD テスト

### 1.5 動作確認テスト ✅

`lib/sqlite3/os32_sqlite_test.c`:

| テスト項目 | 結果 |
|-----------|------|
| `sqlite3_initialize()` (MEMSYS5 + VFS 登録) | ✅ |
| `sqlite3_malloc()` / `sqlite3_free()` | ✅ |
| `sqlite3_memory_used()` | ✅ |
| `sqlite3_open(":memory:", &db)` | ✅ |
| `sqlite3_exec(db, "SELECT 1", ...)` | ✅ |
| `sqlite3_close(db)` | ✅ |
| ブート後のシェル正常起動 | ✅ |

### 1.6 発見した問題と対策

| 問題 | 原因 | 対策 |
|------|------|------|
| Page Fault 0x3ECxxx | `.sqlite_bss` 未初期化 (NOLOAD) | `vfs_read` 後に `memset` でゼロクリア |
| `--gc-sections` で SQLite コード消失 | EXCLUDE_FILE だけでは不十分 | `KEEP()` ディレクティブ追加 |
| NHD ext2 破損 (`Structure needs cleaning`) | ループデバイスの不適切な再利用 | `losetup -D` → クリーン `init` |
| `.sqlite_text` から `kprintf(va_args)` で Page Fault | 原因調査中 (Phase 2 では影響なし) | テスト関数では `tvram_putchar_at` を使用 |

### 1.7 デプロイ構成

```yaml
# deploy.yaml (抜粋)
- host: sqlite.bin
  guest: /sys/sqlite.bin
  tags: [core]
```

```c
/* include/config.h */
#define SYS_SQLITE_BIN  "/sys/sqlite.bin"
```

---

## Phase 2: KAPI + IPC レイヤー 🔲 次

### 2.0 事前作業: テストコードの整理

- [ ] `os32_sqlite_test()` をブート時実行から除外 (条件付きに変更)
- [ ] ファイル DB テスト (ext2 上の永続 DB) の実施

### 2.1 kapi_db.c 実装

外部プログラムは KAPI 関数テーブル経由で SQLite を操作する。
カーネル内に DB 接続スロット (最大4個) を管理。

```
外部プログラム                カーネル
─────────────              ─────────────
db_open("/db/app.db")  ───→  kapi_db_open()
   → handle=0                  → sqlite3_open() → slot[0]
db_exec(0, "CREATE...")  ──→  kapi_db_exec()
   → rc=0                      → sqlite3_exec(slot[0].db, ...)
db_close(0)            ───→  kapi_db_close()
                                → sqlite3_close(slot[0].db)
```

**KAPI 関数一覧 (7個):**

| 関数名 | プロトタイプ | 説明 |
|--------|-------------|------|
| `db_open` | `int(const char *path)` | DB オープン → ハンドル (-1=失敗) |
| `db_close` | `void(int handle)` | DB + ステートメント クローズ |
| `db_exec` | `int(int handle, const char *sql)` | 結果不要の SQL 実行 |
| `db_prepare` | `int(int handle, const char *sql)` | プリペアドステートメント作成 |
| `db_step` | `int(int handle)` | ステートメント実行 (ROW/DONE) |
| `db_column_int` | `int(int handle, int col)` | カラム値取得 (整数) |
| `db_column_text` | `const char *(int handle, int col)` | カラム値取得 (文字列) |

### 2.2 kapi.json 更新

- `version`: 28 → 29
- `includes` に `"kapi_db.h"` を追加
- `api` に DB 関連 7 関数を追加
- `make clean` → `make all` で全プログラム再ビルド

### 2.3 libos32db (ユーザー空間ライブラリ)

`programs/libos32db/libos32db.h` — `api->db_open()` 等を呼ぶシンラッパー。

### 2.4 db_test.c (テストプログラム)

KAPI 経由で `CREATE TABLE` / `INSERT` / `SELECT` の一連の動作を確認。

---

## Phase 3: 堅牢化 🔲 未着手

- [ ] `exec_exit()` クリーンアップ統合 (`db_cleanup_all()`)
- [ ] fsync 実装確認 (INSERT → 電源断 → 再起動 → SELECT)
- [ ] エッジケーステスト (NOMEM, 二重close, 不正パス)
- [ ] deploy.yaml 更新 (`/db/` ディレクトリ作成)

---

## メモリマップ (SQLite 関連)

```
0x18A000 ┌──────────────────────────┐ __sqlite_start
         │  .sqlite_text  (328KB)   │  SQLite エンジン + VFS + テスト
0x1DC060 ├──────────────────────────┤
         │  .sqlite_rodata (32KB)   │  文字列定数, テーブル等
0x1E3E60 ├──────────────────────────┤
         │  .sqlite_data   (5KB)    │  sqlite3Config, VFS構造体等
0x1E51D8 ├──────────────────────────┤ __sqlite_data_end
         │  .sqlite_bss   (100KB)   │  mem5, sqlite_mem_pool[100KB]
0x1FE560 └──────────────────────────┘ __sqlite_end
0x1FFFFF ─── カーネル拡張域上限 ─── (残り約 7KB マージン)
```

---

## 成果物ファイル一覧

| ファイル | 説明 | Phase |
|---------|------|-------|
| `lib/sqlite3/sqlite3.c` | OMIT付きamalgamation | 0 ✅ |
| `lib/sqlite3/sqlite3.h` | SQLite ヘッダ | 0 ✅ |
| `lib/sqlite3/os32_sqlite_config.h` | 全コンパイルマクロ (32個OMIT) | 0 ✅ |
| `lib/sqlite3/os32_sqlite_vfs.c` | カスタム VFS + MEMSYS5 初期化 | 1 ✅ |
| `lib/sqlite3/os32_sqlite_vfs.h` | VFS ヘッダ | 1 ✅ |
| `lib/sqlite3/os32_sqlite_test.c` | カーネル内テスト | 1 ✅ |
| `build/os32.ld` | リンカスクリプト (SQLite分離配置) | 1 ✅ |
| `kapi/kapi_db.c` | KAPI ラッパー | 2 🔲 |
| `kapi/kapi_db.h` | KAPI DB ヘッダ | 2 🔲 |
| `programs/libos32db/libos32db.h` | ユーザー空間ライブラリ | 2 🔲 |
| `programs/libos32db/libos32db.c` | ユーザー空間ライブラリ | 2 🔲 |
| `programs/tests/db_test.c` | KAPI経由テストプログラム | 2 🔲 |

---

## 工数見積もり

| Phase | 工数 | 累計 |
|-------|------|------|
| Phase 0: 事前準備 | ✅ 完了 | — |
| Phase 1: カーネル統合基盤 | ✅ 完了 (2セッション) | — |
| Phase 2: KAPI + IPC | 3-5日 | 3-5日 |
| Phase 3: 堅牢化 | 2-3日 | 5-8日 |
| **合計** | | **5-8日** |

---

## 将来拡張候補

### 中優先度

- [ ] **`db_exec_bind()` — プリペアドステートメント + バインド変数**
- [ ] **ext2 truncate 実装** (`VACUUM`, `journal_mode=TRUNCATE`)
- [ ] **kprintf va_args 問題の調査** (`.sqlite_text` からの呼び出し)

### 低優先度

- [ ] **sqlite3 シェルコマンド** (`.tables`, `.schema`, `.dump`)
- [ ] **KAPI: db_mem_used()** (メモリ使用量モニタリング)
- [ ] **複数接続数の拡張** (`DB_MAX_CONNECTIONS` 増加)

---

*Implementation Phases — 2026-04-24 Phase 1 完了, Phase 2 設計確定*
