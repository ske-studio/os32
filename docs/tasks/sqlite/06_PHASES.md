# タスク06: 実装フェーズ計画・TODO

## 進捗サマリ

| Phase | 内容 | 状態 |
|-------|------|------|
| Phase 0 | 事前準備 (amalgamation生成, OMIT検証, コンパイル確認) | ✅ 完了 |
| Phase 1 | カーネル統合基盤 (分離配置, VFS, メモリDB動作確認) | ✅ 完了 |
| Phase 2 | KAPI + IPC レイヤー (kapi_db, libos32db, db_test) | ✅ 完了 (db_test 7/7 通過) |
| Phase 3 | 堅牢化 (ファイルDB, fsync, MEMSYS5モニタリング) | ✅ 完了 (db_test 9/9 通過) |

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
4. `os32_sqlite_test()` — メモリDB CRUD テスト (SQLITE_BOOT_TEST 時のみ)

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

## Phase 2: KAPI + IPC レイヤー ✅ 完了

### 2.0 事前作業: テストコードの整理 ✅

- [x] `os32_sqlite_test()` をブート時実行から除外 (`#ifdef SQLITE_BOOT_TEST` ガード)

### 2.1 kapi_db.c 実装 ✅

`kapi/kapi_db.c` — DB接続スロット管理 + KAPI ラッパー:

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

**KAPI 関数一覧 (9個):**

| 関数名 | プロトタイプ | 説明 |
|--------|-------------|------|
| `db_open` | `int(const char *path)` | DB オープン → ハンドル (-1=失敗) |
| `db_close` | `int(int handle)` | DB + ステートメント クローズ |
| `db_exec` | `int(int handle, const char *sql)` | 結果不要の SQL 実行 |
| `db_prepare` | `int(int handle, const char *sql)` | クエリ準備 + 最初の行取得 |
| `db_step` | `int(int handle)` | 次の行を取得 (ROW/DONE) |
| `db_column_int` | `int(int handle, int col)` | カラム値取得 (整数) |
| `db_column_text` | `const char *(int handle, int col)` | カラム値取得 (文字列) |
| `db_finalize` | `int(int handle)` | ステートメント手動 finalize |
| `db_last_error` | `const char *(int handle)` | SQLite エラーメッセージ取得 |

**制約:** SQL 文字列は最大 1024 バイト (`SQL_COPY_BUF_SIZE`)。超過分は切り捨てられる。

**共有メモリ IPC プロトコル:**

`db_prepare()` / `db_step()` 呼び出し後、`MEM_SHM_BASE` (0x201000) に以下の構造で結果が書き込まれる:

```
+0x0000  DB_ResultHeader (12 bytes)
         status / column_count / error_offset
+0x000C  DB_ColumnInfo[0..N] (各12 bytes)
         type / length / data_offset
+0x????  データ領域
         INT: 4バイト, TEXT: NUL終端文字列, etc.
```

### 2.2 kapi.json 更新 ✅

- `version`: 28 → 30
- `includes` に `"kapi_db.h"` を追加
- `api` に DB 関連 9 関数を追加 (db_finalize, db_last_error 含む)

### 2.3 os32_kapi_shared.h 更新 ✅

- `KAPI_VERSION` を 30 に更新
- DB API 共有定数 (`DB_STATUS_*`, `DB_TYPE_*`, `DB_MAX_CONNECTIONS`, `DB_SHM_BLOCK_SIZE`) を追加
- `DB_ResultHeader` / `DB_ColumnInfo` 構造体を追加

### 2.4 exec_exit() 統合 ✅

`exec/exec.c` の `exec_exit()` クリーンアップシーケンスに `db_cleanup_all()` を追加:

```
(1) FD リダイレクト解除
(2) 全オープン FD クローズ
(3) パイプバッファ解放
(4) 共有メモリ解放
(5) サウンドエンジンクリーンアップ
(6) ★ SQLite DB リソースクリーンアップ  ← Phase 2 で追加
```

### 2.5 libos32db (ユーザー空間ライブラリ) ✅

`programs/libos32db/`:
- `libos32db.h` — 外部プログラム向け API ヘッダ
- `libos32db.c` — KAPI 呼び出し + 共有メモリ直接参照

追加 API:
- `db_finalize(h)` — ステートメント手動 finalize
- `db_last_error(h)` — SQLite エラーメッセージ取得 (KAPI 経由)
- `db_column_blob(col)` — BLOB データポインタ (共有メモリ直接参照)

### 2.6 db_test.c (テストプログラム) ✅

`programs/tests/db_test.c` — 7つのテストケース:

| テスト | 内容 |
|--------|------|
| Test 1: Memory DB CRUD | `:memory:` で CREATE/INSERT×3/SELECT/close |
| Test 2: Multiple Connections | 2つの独立したメモリDB同時接続 |
| Test 3: Error Handling | 不正テーブル/不正SQL/不正ハンドル |
| Test 4: db_finalize | 結果セット途中放棄 → 新クエリ成功確認 |
| Test 5: db_last_error | エラー後にメッセージ取得 (KAPI + 共有メモリ両方) |
| Test 6: Connection Limit | DB_MAX_CONNECTIONS (4) 超過 → 拒否確認 |
| Test 7: Double Close | 二重 close → -1 返却・クラッシュしないこと |

### 2.7 VFS 堅牢化 ✅

- `xTruncate`: no-op の影響範囲をコメントで文書化 (VACUUM 制約)
- `xDelete`: `vfs_rm` 失敗時の警告ログ追加
- SQL バッファ制限 (1024B) を `kapi_db.h` / `libos32db.h` に明記

### 2.8 ビルド結果 ✅

| 成果物 | サイズ | 状態 |
|--------|--------|------|
| `kernel.bin` | 97KB | ✅ ビルド成功 (kapi_db.c リンク含む) |
| `sqlite.bin` | 373KB | ✅ 変更なし |
| `programs/tests/db_test.bin` | 6KB (api>=30) | ✅ ビルド成功 |

### 2.9 残作業

- [x] **NP21/W 再起動 → hsync → `db_test` 実行で動作検証** (7/7 全テスト通過)
- [ ] ファイル DB テスト (`/db/test.db`) の実施 (Phase 3 に移行可)

---

## Phase 3: 堅牢化 ✅ 完了

- [x] `exec_exit()` クリーンアップ統合 (`db_cleanup_all()`) — Phase 2 で実装済み
- [x] `kapi_db.c` プロダクション品質化 (probe コード除去, デバッグログ削除)
- [x] `journal_mode=OFF` → `journal_mode=DELETE` に変更 (クラッシュリカバリ有効化)
- [x] VFS `xOpen` の `O_CREAT` フラグ値修正 (`0x0200` → `0x0100` KAPI_O_CREAT)
- [x] `deploy.yaml` に `/db/` ディレクトリ追加
- [x] ファイル DB テスト: CREATE/INSERT/close/reopen/SELECT/UPDATE/DELETE 永続化確認
- [x] `db_mem_used()` KAPI 追加 (MEMSYS5 使用量モニタリング, KAPI_VERSION 30→31)
- [x] `libos32db` に `db_mem_used()` ラッパー追加
- [x] `db_test` Test 8 (File DB Persistence) + Test 9 (Memory Usage Monitoring) 追加
- [x] db_test 9/9 全テスト通過確認

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
| `kapi/kapi_db.h` | KAPI DB ヘッダ | 2 ✅ |
| `kapi/kapi_db.c` | KAPI ラッパー (DB接続スロット管理) | 2 ✅ |
| `programs/libos32db/libos32db.h` | ユーザー空間ライブラリ ヘッダ | 2 ✅ |
| `programs/libos32db/libos32db.c` | ユーザー空間ライブラリ 実装 | 2 ✅ |
| `programs/tests/db_test.c` | KAPI経由テストプログラム | 2 ✅ |

---

## 工数見積もり

| Phase | 工数 | 累計 |
|-------|------|------|
| Phase 0: 事前準備 | ✅ 完了 | — |
| Phase 1: カーネル統合基盤 | ✅ 完了 (2セッション) | — |
| Phase 2: KAPI + IPC | ✅ 完了 (1セッション) | — |
| Phase 3: 堅牢化 | ✅ 完了 (1セッション) | — |
| **合計** | | **完了** |

---

## 将来拡張候補

### 中優先度

- [ ] **`db_read_blob()` — 巨大BLOB分割読み込み**
- [ ] **ext2 truncate 実装** (`VACUUM`, `journal_mode=TRUNCATE`)
- [ ] **kprintf va_args 問題の調査** (`.sqlite_text` からの呼び出し)

### 低優先度

- [ ] **sqlite3 シェルコマンド** (`.tables`, `.schema`, `.dump`)
- [ ] **KAPI: db_mem_used()** (メモリ使用量モニタリング) ← ✅ Phase 3 で実装済み
- [ ] **複数接続数の拡張** (`DB_MAX_CONNECTIONS` 増加)

---

*Implementation Phases — 2026-04-27 Phase 3 堅牢化完了 (db_test 9/9 通過)*
