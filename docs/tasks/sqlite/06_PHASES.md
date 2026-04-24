# タスク06: 実装フェーズ計画・TODO

## 完了済みフェーズ

| Phase | 内容 | 状態 |
|-------|------|------|
| — | (まだ着手なし) | — |

---

## 実装フェーズ

### Phase 0: 事前準備

- [ ] **amalgamation 生成**
  - sqlite-src-3530000 からビルド、または公式サイトからダウンロード
  - `sqlite3.c` + `sqlite3.h` を `lib/sqlite3/` に配置

- [ ] **C89 互換パッチ**
  - `//` コメント → `/* */` 変換
  - クロスコンパイル試行 (`i386-elf-gcc -std=gnu89 -Wno-long-long`)
  - コンパイルエラーの修正パッチ作成

- [ ] **ディレクトリ構成作成**
  ```
  lib/sqlite3/
      sqlite3.c          ← amalgamation (パッチ適用済み)
      sqlite3.h
      os32_sqlite_vfs.c   ← カスタム VFS
      os32_sqlite_vfs.h
  kapi/
      kapi_db.c           ← KAPI ラッパー (Phase 2)
  programs/libos32db/
      libos32db.h          ← ユーザー空間ライブラリ (Phase 2)
      libos32db.c
  programs/tests/
      db_test.c            ← PoC テストプログラム (Phase 1)
  ```

### Phase 1: 外部プログラム PoC (3-5日)

SQLite をカーネルに統合せず、外部プログラムとして直接リンクする。
カーネル変更ゼロでコンパイル・VFS 動作を検証。

- [ ] **sqlite3.c のクロスコンパイル**
  - OMIT マクロ + リソース制限マクロ適用
  - MEMSYS5 を `exec_heap` (プログラムヒープ) 上に固定プール配置
  - ビルド成功の確認 (カーネル非統合、--heap 十分大きく設定)

- [ ] **os32_sqlite_vfs.c 実装**
  - xOpen / xClose / xRead / xWrite / xFileSize
  - xSync (ext2_sync 呼び出し)
  - xDelete / xAccess / xFullPathname
  - xTruncate (no-op)
  - xRandomness / xSleep / xCurrentTime
  - xLock / xUnlock / xCheckReservedLock (no-op)

- [ ] **db_test.c 実装**
  - `sqlite3_open_v2()` でインメモリ DB を開く
  - `CREATE TABLE` / `INSERT` / `SELECT` / `DROP TABLE`
  - コンソールに結果出力
  - メモリ使用量 (`sqlite3_memory_used()`) 表示

- [ ] **ファイル DB テスト**
  - ext2 上に DB ファイル作成 (`/db/test.sqlite`)
  - INSERT → 再起動 → SELECT でデータ永続性確認
  - HostDrvFS 上でも同様のテスト (`/host/test.sqlite`)

- [ ] **計測**
  - コンパイル後の .text サイズ計測
  - `sqlite3_memory_used()` のピーク計測
  - INSERT / SELECT の所要時間 (tick_count ベース)

### Phase 2: カーネル統合 (5-7日)

Phase 1 の成果物をカーネルに移植。KAPI + IPC レイヤーを実装。

- [ ] **sqlite3.c をカーネルにリンク**
  - Makefile の `C_KERNEL` リストに追加
  - MEMSYS5 プールをカーネル予約域 (0x18A000〜) に配置
  - リンカスクリプトの確認 (BSS 拡大によるヒープ干渉チェック)
  - `make clean && make all` でビルド確認

- [ ] **kapi_db.c 実装**
  - `db_open()` / `db_close()`
  - `db_exec()` / `db_step()` / `db_finalize()`
  - `db_read_blob()`
  - `db_last_error()`
  - 共有メモリへの結果書き込みロジック

- [ ] **kapi.json 更新**
  - DB 関連 KAPI 関数 7 個追加
  - `KAPI_VERSION` を 29 に更新
  - `make clean && make all` でコード生成 + 全プログラム再ビルド

- [ ] **libos32db 実装**
  - `db_open()` / `db_close()` ラッパー
  - `db_exec()` / `db_query()` / `db_step()`
  - `db_column_int()` / `db_column_text()` / `db_column_blob()`
  - `db_read_blob()` (巨大データ分割読み込み)
  - `db_errmsg()`

- [ ] **db_test.c を KAPI 版に移行**
  - 直接 SQLite 呼び出し → `libos32db` 呼び出しに変更
  - 同等のテスト結果を確認

### Phase 3: 堅牢化 (2-3日)

- [ ] **exec_exit() クリーンアップ統合**
  - `db_cleanup_all()` 追加
  - 強制終了テスト (db_open → フォールト発生 → スロット解放確認)

- [ ] **fsync 実装確認**
  - INSERT → sync → 電源断 (NP21/W 強制終了) → 再起動 → SELECT で確認
  - ジャーナルファイル残存時の自動ロールバック確認

- [ ] **エッジケーステスト**
  - MEMSYS5 プール枯渇時の SQLITE_NOMEM 返却確認
  - 非常に長い SQL 文 (MAX_SQL_LENGTH=10000 超過) のエラー確認
  - 存在しないパスへの db_open エラー確認
  - 二重 db_close の安全性確認

- [ ] **deploy.yaml 更新**
  - `/db/` ディレクトリの作成
  - db_test.bin の配置パス追加

---

## 成果物ファイル一覧

| ファイル | 説明 | Phase |
|---------|------|-------|
| `lib/sqlite3/sqlite3.c` | amalgamation (パッチ済み) | 0 |
| `lib/sqlite3/sqlite3.h` | SQLite ヘッダ | 0 |
| `lib/sqlite3/os32_sqlite_vfs.c` | カスタム VFS | 1 |
| `lib/sqlite3/os32_sqlite_vfs.h` | VFS ヘッダ | 1 |
| `programs/tests/db_test.c` | テストプログラム | 1 |
| `kapi/kapi_db.c` | KAPI ラッパー | 2 |
| `programs/libos32db/libos32db.h` | ユーザー空間ライブラリ | 2 |
| `programs/libos32db/libos32db.c` | ユーザー空間ライブラリ | 2 |

---

## 工数見積もり

| Phase | 工数 | 累計 |
|-------|------|------|
| Phase 0: 事前準備 | 1-2日 | 1-2日 |
| Phase 1: 外部プログラム PoC | 3-5日 | 4-7日 |
| Phase 2: カーネル統合 | 5-7日 | 9-14日 |
| Phase 3: 堅牢化 | 2-3日 | 11-17日 |
| **合計** | | **11-17日** |

---

## 将来拡張候補

### 中優先度

- [ ] **`db_exec_bind()` — プリペアドステートメント + バインド変数**
  - SQL インジェクション防止
  - 同じクエリの繰り返し実行の高速化
  - 共有メモリにバインド値を配置するプロトコル拡張

- [ ] **ext2 truncate 実装**
  - `VACUUM` サポート
  - `journal_mode=TRUNCATE` サポート
  - 見積もり: 2.5日

### 低優先度

- [ ] **sqlite3 シェルコマンド**
  - `.tables`, `.schema`, `.dump` 相当の機能
  - シェル内蔵コマンド `sql` として実装
  - インタラクティブ SQL 実行

- [ ] **KAPI: db_mem_used()**
  - SQLite メモリ使用量のモニタリング
  - デバッグ/プロファイリング用

- [ ] **複数接続数の拡張**
  - `DB_MAX_CONNECTIONS` の増加
  - 必要になった時点で定数変更のみ

---

*Implementation Phases — 2026-04-24*
