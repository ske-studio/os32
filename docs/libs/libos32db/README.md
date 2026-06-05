# libos32db — SQLiteデータベースアクセス・ライブラリ

## 1. 概要

`libos32db` は、OS32カーネルに統合された **SQLite3 データベース**に対して、ユーザー空間プログラムからSQLクエリを発行し、結果セットを取得するためのAPIラッパーライブラリです。

OS32ではゲーム内のパラメータ、テキスト会話データ、マップ定義などをデータベースで管理しており、本ライブラリがデータベースアクセスの中心として機能します。

---

## 2. アーキテクチャ

カーネル内に常駐する SQLite エンジンに対し、`KernelAPI`（システムコール）を介してやり取りを行います。

```
[ユーザー空間プログラム] (libos32db API呼出)
         ↓
  KernelAPI (sys_db_open / sys_db_prepare / sys_db_step 等)
         ↓
[OS32カーネル] (SQLite VFS / SQL実行エンジン)
         ↓
   /db/hoge.db (ファイルシステム上のデータベースファイル)
```

カーネルは同時に開くことができるデータベース接続数を**最大8スロット**（接続）に制限しています。

---

## 3. 主要API

### 3.1 接続・切断
*   `int db_open(const char *path)`
    *   データベースファイルを開きます。空いている接続スロット（0〜7）を確保して、スロットIDを返します。失敗時は負のエラーコードを返します。
*   `void db_close(int slot)`
    *   指定した接続スロットをクローズし、リソースを解放します。

### 3.2 SQL実行・準備（ステートメント）
*   `int db_prepare(int slot, const char *sql)`
    *   SQL文をコンパイルし、ステートメントを実行準備状態にします。
*   `int db_step(int slot)`
    *   準備されたSQLを実行し、次の行（結果レコード）へ進めます。
    *   戻り値: `KAPI_SQLITE_ROW`（次の行あり）、`KAPI_SQLITE_DONE`（実行完了）、負値（エラー）。
*   `int db_finalize(int slot)`
    *   現在のステートメント実行をクローズし、スロットを解放します。

### 3.3 結果データの取得
`db_step` で `ROW` が返ってきた後、列のデータを型ごとに取得できます。
*   `int db_column_int(int slot, int col_idx)`: 列のデータを `int` 整数として取得します。
*   `const char *db_column_text(int slot, int col_idx)`: 列のデータを文字列ポインタとして取得します。
*   `int db_column_count(int slot)`: 結果セットの列数を取得します。

---

## 4. 使用例

```c
#include "libos32db.h"

void query_sample(void) {
    int slot;
    int rc;

    /* データベースオープン */
    slot = db_open("/db/map.db");
    if (slot < 0) {
        // エラー処理
        return;
    }

    /* クエリの準備 */
    rc = db_prepare(slot, "SELECT id, name, width FROM maps WHERE id = 1;");
    if (rc == 0) {
        /* 結果レコードの読み進め */
        while (db_step(slot) == KAPI_SQLITE_ROW) {
            int id = db_column_int(slot, 0);
            const char *name = db_column_text(slot, 1);
            int width = db_column_int(slot, 2);
            
            // データを利用する処理...
        }
        db_finalize(slot);
    }

    /* データベースクローズ */
    db_close(slot);
}
```

---

## 5. 関連ドキュメント
さらに詳細なSQLite統合アーキテクチャについては、[SQLITE_INTEGRATION.md](../../SQLITE_INTEGRATION.md) を参照してください。
