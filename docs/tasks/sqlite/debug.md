# OS32 SQLite KAPI 統合 デバッグ調査報告

## 1. 発生していた事象の再確認
`db_test` 実行時に `sqlite3_open` を呼び出した直後、以下の現象が発生していました。
- **事象A**: `Fault EIP: 0x1E7B9` (UTF-16変換処理 `kutf16le_to_utf8` の内部) での書き込みページフォールト
- **事象B**: その後、完全なハングアップ（Page Faultすら表示されない状態）への変化

## 2. 判明した事実と根本原因

### ① 画面出力が上書きされていた問題（事象Aのフェイク）
スクリーンショットに `Fault EIP: 0x0001E7B8tegration test` という文字化けが表示されていました。
調査の結果、例外ハンドラ (`isr_handlers.c`) が **描画行を「14行目」にハードコーディングしていた** ことが判明しました。
これにより、`db_test` が出力したログ (`db_test: KAPI SQLite integration test`) の上に Page Fault エラーが上書き描画され、一見すると直前のログが表示されていないように見えていました。
（※このハンドラの問題は修正済みです）

### ② SQLite メモリプール（ヒープ）の未初期化問題（真の原因）
SQLite はカーネル内で `sqlite_mem_pool` (100KB) という独自の配列を `.sqlite_bss` セクションに確保し、これを MEMSYS5 アロケータとして使用しています。
しかし、カーネルのエントリポイントである `kentry.asm` を確認したところ、**通常の `.bss` セクションしかゼロクリアしていませんでした。**

```assembly
;; kentry.asm の該当箇所
mov     edi, __bss_start
mov     ecx, __bss_end
...
rep     stosd   ;; ここで通常BSSのみクリアされる
```

SQLite用の `.sqlite_bss` セクションは `kernel.ld` で手動で配置を定義していますが、カーネル起動時に **ゼロクリア処理が一切行われていない** ため、ヒープ領域に過去のメモリのゴミ（ランダムな値）が残存したまま SQLite が初期化されています。

その結果、SQLite内部で `sqlite3_malloc` が呼び出された際、不正なフリーブロックポインタを辿ってランダムなアドレスにアクセス（Page Fault）するか、破損したリストを無限ループで辿り続ける（ハングアップ）という不定な挙動を引き起こしていました。

**※ ただし `kernel.c` L344-348 で `vfs_read` 後に `memset` による `.sqlite_bss` のゼロクリアが実装されているため、この問題は既に対処されている可能性がある。**

## 3. デバッグ進捗 (2026-04-24)

### 修正1: `kapi_db_open` の path コピー漏れ (適用済み)

`kapi_db_exec()` では SQL 文字列を `kstrncpy` でカーネルバッファにコピーしていたが、
`kapi_db_open()` では `path` 引数を **コピーせずに直接 `sqlite3_open()` に渡していた**。
外部プログラム空間 (`0x400000+`) にある文字列を SQLite 拡張域 (`.sqlite_text`, `0x18A000+`) の
コードが読み取る際に、ページングの問題でハングアップしていた。

**修正**: `kapi/kapi_db.c` に `path_copy_buf[256]` を追加し、`sqlite3_open` に渡す前に
カーネルバッファにコピーするように変更。

**結果**: ハングアップ（事象B）は解消。ただし別の Page Fault が発生。

### テスト結果: 修正1適用後

```
Fault Addr: 0x003FAE00
Error Code: 0x00000002 (WRITE to Not-Present page)
Fault EIP:  0x00DFFA64
```

- `0x003FAE00` はシェル帯域終端〜プログラム空間の間 (`0x380000-0x3FFFFF`) で NOT PRESENT に設定されている領域
- `Fault EIP: 0x00DFFA64` は SQLite拡張域 (`0x18A000-0x1FE300`) を大幅に超えた無効なアドレス
- これは **SQLite 内部の関数ポインタが破損** している強い証拠

### テスト結果: カーネル内ブートテスト

`SQLITE_BOOT_TEST` を有効化して `os32_sqlite_test()` をカーネル起動時に実行したところ、
**カーネル自体がハングアップしてシェルが起動しなかった**。

→ **KAPI IPC の問題ではなく、SQLite エンジン自体がカーネルモードでもハングする。**

### シンボルアドレス情報

```
0018a000 T __sqlite_start     (SQLiteコード開始)
001e4f78 D __sqlite_data_end  (.sqlite_data 終端)
001e5300 b sqlite_mem_pool    (MEMSYS5 100KBプール)
001fe300 B __sqlite_end       (.sqlite_bss 終端)
```

sqlite.bin サイズ: 372,600 bytes (= .text + .rodata + .data の合計と完全一致)

## 4. 現在の仮説と次のステップ

### 仮説A (最有力): SQLite初期化の順序問題

`os32_sqlite_init()` は `sqlite3_config(SQLITE_CONFIG_HEAP, ...)` → `sqlite3_initialize()` の順で
呼び出しているが、sqlite3_initialize の内部処理で何らかのコールバック/関数ポインタテーブルが
不正なアドレスを指している可能性がある。

**検証方法**: `os32_sqlite_test()` を段階的に実行し、どのステップでハングするか特定する:
1. `sqlite3_malloc(256)` のみ実行
2. `sqlite3_open(":memory:", &db)` のみ実行
3. `sqlite3_exec(db, "SELECT 1", ...)` 実行

### 仮説B: sqlite.bin のロード不整合

`objcopy -O binary` で抽出した sqlite.bin のロードアドレスが、リンカスクリプトの VMA と
一致しない可能性。ロードされたバイナリの先頭バイトを読み取って検証が必要。

### 仮説C: ページング初期化後の TLB 不整合

SQLite 初期化はページング有効化 (L287) の後に実行されるが、`vfs_read` で `0x18A000+` に
バイナリをロードした後に TLB がフラッシュされていない可能性。

**次のアクション**: ユーザーと方針を協議。
