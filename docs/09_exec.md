## 第9部 外部プログラム実行 (exec)

OS32Xバイナリ形式の実行ファイルをext2から0x400000にロードし、
KernelAPIポインタを引数として実行する。

詳細は **[KAPI_SPEC.md](../KAPI_SPEC.md)** を参照。

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| ソースファイル | `exec/exec.c` / `exec/exec.h` / `exec/exec_heap.c` |
| KAPIラッパー | `kapi/kapi_*.c` (自動生成分 + 手動分) |
| KAPIテーブルアドレス | 0x189000 |
| ロードアドレス | 0x400000 (固定・プロセス間共有) |
| プログラム専用ヒープ | 動的配置 (sbrk_heap_limit, exec_heap 管理下) |
| ネスト実行 | 非サポート (execve 方式によるプロセスの完全置換。終了時はカーネルに復帰し無名シェルを再起動) |
| カーネル側規約 | GCC (System V) + `__cdecl` ラッパー |
| 外部プログラム規約 | System V i386 ABI (スタック渡し) |
| 現在のAPIバージョン | **22** |
| プログラム専用スタック | 動的配置 (128KB, メモリ終端付近から下方に展開) |
| スタック保護 | GUARD B (Not-Present) ガードページによる保護 |

### 実行方式

```bash
> ./program.bin     # 直接実行
> program           # 未知コマンド → 自動的に *.bin を補完探索
> exec program.bin  # 明示的exec
```

### 主な外部化済みプログラム

### 実行ステータスコード (`exec_status_t`)

`exec_run` 関数は実行結果として、`exec_status_t` 列挙型の値を返します (定義: `exec.h`)。
※ 注意：現在の `execve` 置換方式では、プログラムから別のプログラムを呼び出した場合、成功すると呼び出し元の空間は破棄されるためエラー以外でリターンすることはありません。終了時はカーネルへ直接復帰します。

| ステータス | 値 | 説明 |
|---------|---------|------|
| `EXEC_SUCCESS` | `0` | 正常終了 |
| `EXEC_ERR_GENERAL` | `-1` | 一般的なエラー（ロード失敗等） |
| `EXEC_ERR_FAULT` | `-2` | 例外やフォールトによるプロセスの異常終了 |
| `EXEC_ERR_NOT_FOUND` | `-3` | 指定されたOS32Xバイナリが見つからない |
| `EXEC_ERR_NOMEM` | `-4` | メモリサイズ超過、ページング領域確保失敗等のメモリ不足 |
| `EXEC_ERR_INVALID` | `-5` | ヘッダマジック不一致やバージョン非互換等の不正なバイナリ |

| プログラム | ソース | 説明 |
|---------|---------|------|
| shell | `programs/shell/` | システム標準シェル (階層化モジュール構造) |
| vz | `programs/vz/` | VZ Editor移植版 |
| skk_test| `programs/skk/` | SKKフロントエンド |
| gfx_demo| `programs/gfx_demo.c` | libos32gfx グラフィックスデモ |
| spr_test| `programs/spr_test.c` | スプライト描画テスト |
| bench | `programs/bench/` | ベンチマークプログラム |
| install | `programs/install.c` | HDDインストーラ |
| test2-4 | `programs/` | APIテスト・システム検証用 |
