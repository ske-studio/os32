# SQLite カーネル統合

OS32 カーネルへの SQLite 3.53.0 統合の設計・実装リファレンス。

> **出典**: このドキュメントは `docs/tasks/sqlite/` (全7部, 2026-04-24 ~ 04-27 実装完了)
> の設計・実装ドキュメントを集約したものです。
> 元ファイルは `docs/tasks/_archived/sqlite/` に保存されています。

---

## §1. アーキテクチャ概要

SQLite エンジンをカーネルに統合し、外部プログラムからは KernelAPI +
共有メモリ経由で操作するアーキテクチャ。

### スペック

| 項目 | 値 |
|------|-----|
| SQLite バージョン | 3.53.0 (sqlite-src-3530000) |
| ビルド形式 | amalgamation (sqlite3.c 単一ファイル) |
| OS 統合方式 | カーネルリンク + KAPI 公開 |
| VFS | カスタム (SQLITE_OS_OTHER=1) |
| スレッド安全性 | 無効 (SQLITE_THREADSAFE=0) |
| メモリ管理 | MEMSYS5 固定プール (200KB) |
| ジャーナルモード | DELETE (デフォルト) |
| DB 接続上限 | 8 (`DB_MAX_CONNECTIONS`) |
| ターゲット FS | ext2 (メイン) + HostDrvFS |
| 最適化 | `-Os` 必須 (.text 360KB) |

### システム構成図

```
┌─────────────────────────────────────────────────────┐
│  外部プログラム (0x400000〜)                          │
│  ┌──────────────────────┐                            │
│  │  libos32db.h         │ ← ユーザー空間ラッパー      │
│  │    db_open()          │    共有メモリのパース/構築  │
│  │    db_exec()          │                            │
│  │    db_step()          │                            │
│  │    db_column_int()    │                            │
│  │    db_column_text()   │                            │
│  │    db_close()         │                            │
│  │    db_read_blob()     │                            │
│  └──────────┬───────────┘                            │
│             │ KAPI 呼び出し (api->db_xxx)             │
├─────────────┼────────────────────────────────────────┤
│  カーネル空間                                         │
│             ▼                                        │
│  ┌──────────────────┐    ┌───────────────────────┐   │
│  │  kapi_db.c        │──▶│  SQLite3 エンジン      │   │
│  │  (KAPI ラッパー)   │    │  (sqlite3.c 統合)     │   │
│  │                    │    │  MEMSYS5 固定プール    │   │
│  └──────────────────┘    └──────────┬────────────┘   │
│                                     │                 │
│                          ┌──────────▼────────────┐   │
│                          │  os32_sqlite_vfs.c     │   │
│                          │  (カスタム VFS)         │   │
│                          └──────────┬────────────┘   │
│                                     │                 │
│                          ┌──────────▼────────────┐   │
│                          │  OS32 VFS レイヤー     │   │
│                          │  (vfs.c → ext2/hdf)   │   │
│                          └───────────────────────┘   │
└──────────────────────────────────────────────────────┘
```

---

## §2. コンパイル設定

### 必須オプション

```makefile
SQLITE_CFLAGS = \
    -DSQLITE_THREADSAFE=0 \
    -DSQLITE_OS_OTHER=1 \
    -DSQLITE_TEMP_STORE=3 \
    -DSQLITE_DISABLE_LFS \
    -DSQLITE_DQS=0
```

### 機能削減オプション (OMIT 系)

```makefile
SQLITE_OMIT_FLAGS = \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_OMIT_WAL \
    -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_AUTOINIT \
    -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_VIRTUALTABLE \
    -DSQLITE_OMIT_TRIGGER \
    -DSQLITE_OMIT_AUTHORIZATION \
    -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_TRACE \
    -DSQLITE_OMIT_EXPLAIN \
    -DSQLITE_OMIT_TCL_VARIABLE \
    -DSQLITE_OMIT_COMPLETE \
    -DSQLITE_OMIT_DECLTYPE \
    -DSQLITE_OMIT_JSON
```

> **注意**: `OMIT_CTE` と `OMIT_WINDOWFUNC` は公式amalgamationではパーサー不整合で使用不可。
> OMITマクロ付きでソースからamalgamationを再生成することで対応済み。

### リソース制限

```makefile
SQLITE_LIMIT_FLAGS = \
    -DSQLITE_DEFAULT_CACHE_SIZE=10 \
    -DSQLITE_DEFAULT_PAGE_SIZE=1024 \
    -DSQLITE_MAX_COLUMN=100 \
    -DSQLITE_MAX_SQL_LENGTH=10000 \
    -DSQLITE_MAX_EXPR_DEPTH=100 \
    -DSQLITE_MAX_COMPOUND_SELECT=5 \
    -DSQLITE_MAX_VARIABLE_NUMBER=100 \
    -DSQLITE_MAX_ATTACHED=0
```

### MEMSYS5 メモリプール初期化

```c
static u8 sqlite_mem_pool[200 * 1024];  /* 200KB 固定プール */

void sqlite_engine_init(void)
{
    sqlite3_config(SQLITE_CONFIG_HEAP,
                   sqlite_mem_pool,
                   sizeof(sqlite_mem_pool),
                   64);  /* 最小アロケーション粒度 */
    sqlite3_initialize();
}
```

### C89 互換性

| 問題 | 対策 |
|------|------|
| `//` コメント | `/* */` に変換 (sed スクリプト) |
| `long long` 型 | `-Wno-long-long` で警告抑制 |
| ブロック途中の変数宣言 | `-std=gnu89` でGCC許容 |

### バイナリサイズ

| 構成 | 最適化 | .text | .data | .bss |
|------|--------|-------|-------|------|
| 全OMIT + amalgamation再生成 | `-Os` | **360KB** | 3.7KB | 789B |
| 同上 | `-O2` | 527KB | 3.7KB | 789B |

---

## §3. カスタム VFS

`SQLITE_OS_OTHER=1` によりUnix/Win VFSを除外し、OS32 の VFS レイヤー上に構築。

### ファイルハンドル

```c
typedef struct {
    sqlite3_file base;          /* SQLite 基底構造体 (先頭配置) */
    char path[VFS_MAX_PATH];    /* OS32 VFS 内の相対パス */
    VfsOps *ops;                /* FSドライバ操作テーブル */
    void *fs_ctx;               /* FSドライバコンテキスト */
    u32 file_size;              /* 現在のファイルサイズ */
} Os32File;
```

### VFS操作マッピング

| SQLite VFS | OS32 実装 | 備考 |
|------------|----------|------|
| `xClose` | メモリ解放のみ | FD 未使用 |
| `xRead` | `ops->read_stream()` | オフセット指定 |
| `xWrite` | `ops->write_stream()` | オフセット指定 |
| `xTruncate` | no-op (SQLITE_OK) | DELETE モードでは不要 |
| `xSync` | `ops->sync()` | FS 全体の sync |
| `xFileSize` | `ops->get_file_size()` | 実装済み |
| `xLock/xUnlock` | no-op | シングルタスク |
| `xRandomness` | PIT + RTC + tick_count の LCG | 暗号学的品質不要 |
| `xSleep` | `cpu_delay_us()` | カーネル関数 |
| `xCurrentTime` | `sys_time()` → Julian Day | RTC ベース |

### HostDrvFS 対応

OS32 の VFS レイヤーがマウントポイント解決を透過的に行うため、
SQLite VFS 内で `vfs_route()` を呼ぶだけで ext2/HostDrvFS を自動振り分け。

```
/db/system.sqlite   → ext2 (ルートマウント)
/host/data/app.db   → HostDrvFS (HostDrv マウント時)
```

---

## §4. KAPI / IPC プロトコル

### KAPI 関数

| 関数 | シグネチャ | 用途 |
|------|-----------|------|
| `db_open` | `int (const char *path, int flags)` | DBオープン → ハンドルID返却 |
| `db_close` | `int (int handle)` | DBクローズ + 自動クリーンアップ |
| `db_exec` | `int (int handle, const char *sql)` | SQL実行、結果1行目を共有メモリに |
| `db_step` | `int (int handle)` | 次の行を取得 |
| `db_finalize` | `int (int handle)` | ステートメント手動finalize |
| `db_read_blob` | `int (int handle, int col, u32 offset, u32 size)` | 巨大BLOB分割読み込み |
| `db_last_error` | `const char * (int handle)` | エラーメッセージ取得 |

### 共有メモリ IPC レイアウト

```
+0x0000  ┌─────────────────────────────────┐
         │  DB_RESULT_HEADER (12 bytes)    │
         │    status (4)                    │  DB_STATUS_DONE=0 / ROW=1 / ERROR=-1
         │    column_count (4)              │
         │    error_offset (4)              │
+0x000C  ├─────────────────────────────────┤
         │  DB_COLUMN_INFO[0] (12 bytes)   │
         │    type (4)                      │  INT=1 / FLOAT=2 / TEXT=3 / BLOB=4 / NULL=5
         │    length (4)                    │
         │    data_offset (4)               │
+0x0018  ├─────────────────────────────────┤
         │  DB_COLUMN_INFO[1] ...           │
+0x????  ├─────────────────────────────────┤
         │  データ領域                     │
+0x3FFF  └─────────────────────────────────┘
         (1ブロック = 16KB)
```

### ユーザー空間ラッパー (`libos32db`)

```
programs/libos32db/
    libos32db.h       ← 外部プログラム用ヘッダ
    libos32db.c       ← 共有メモリパースロジック
```

---

## §5. 安全性とクリーンアップ

### アプリケーションクラッシュ対策

`exec_exit()` に `db_cleanup_all()` を統合:
- 未finalize のステートメントを自動 finalize
- 未クローズの DB 接続を `sqlite3_close_v2()` で安全にクローズ
- アプリケーション所有 (`owner == 1`) のスロットのみ対象

### 電源断対策 (fsync)

- `ext2_sync()` で全ダーティデータをフラッシュ
- IDE PIO は `REP OUTSW` + BSY 待ちで完了保証
- エミュレータ環境では実害なし、実機の IDE ライトキャッシュは将来課題

### メモリ枯渇 (OOM) 対策

- MEMSYS5 プール枯渇時は `SQLITE_NOMEM` を返却
- カーネルパニックは発生しない
- アプリ側でエラーハンドリング可能

### 制約と既知の制限

| 制限 | 回避策 |
|------|--------|
| xTruncate 未実装 | journal_mode=DELETE で実害なし |
| 32bit ファイルオフセット (~2GB) | PC-98 環境では十分 |
| ファイルロック不要 | 将来マルチタスク化時に要対応 |
| IDE ライトキャッシュ FLUSH 未実装 | NP21/W では問題なし |

---

## §6. 導入障害サマリ

2026-04-24 〜 04-27 の統合作業で発生した12件の障害。
**SQLite 3.53.0 自体にはバグゼロ** — 全て OS32 カーネル側のインフラに起因。

| # | 障害 | カテゴリ | 教訓 |
|---|------|---------|------|
| 1 | `.sqlite_bss` 未初期化 → #PF | 初期化 | `NOLOAD` セクションは手動ゼロクリア必須 |
| 2 | `--gc-sections` で SQLite コード消失 | リンカ | 間接呼び出しコードは `KEEP()` で保護 |
| 3 | NHD ext2 パーティション破損 | WSL | `losetup -D` を前処理に入れる |
| 4 | `.sqlite_text` → `kprintf(va_args)` #PF | ABI | セクション間で可変引数関数に注意 |
| 5 | Ext2 DIND 読み込み不正 | FS | 268KB超ファイルの初テスト |
| 6 | Ext2 DIND 書き込みデータ破損 | FS | 読み書きは対称的にテスト |
| 7 | VFS 依存のバイナリロード不安定 | 起動 | ブートローダー直接ロードが堅牢 |
| 8 | `sqlite3_exec` 時の #PF | 複合 | 障害 9, 11 の複合問題 |
| 9 | FPU 未初期化 #NM → #PF | HW初期化 | `double` 使用ライブラリには FPU 初期化必須 |
| 10 | 例外ハンドラのスタックオフセット不正 | ISR | PUSHAD 後のオフセット図を描いて検証 |
| 11 | **`kmemset` スタック不整合 → #UD** | ASM | `push`/`pop` の対称性を全パスで保証 |
| 12 | `xOpen` の `O_CREAT` フラグ値不一致 | 定数 | ハードコード禁止、ヘッダ定数を使用 |

---

## §7. 関連ファイル

| ファイル | 役割 |
|---------|------|
| `lib/sqlite3/sqlite3.c` | SQLite amalgamation |
| `lib/sqlite3/os32_sqlite_vfs.c` | カスタム VFS 実装 |
| `kapi/kapi_db.c` | KAPI DB ラッパー |
| `programs/libos32db/` | ユーザー空間ラッパーライブラリ |
| `build/kernel.mk` | SQLite ビルドルール |

---

*集約元: docs/tasks/sqlite/ (00_INDEX - 07_OBSTACLES + sqlite_step_debug_report)*
*Last Updated: 2026-05-04*
