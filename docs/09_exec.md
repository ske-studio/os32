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
| ネスト実行 | 最大4段 (MAX_EXEC_NEST)。Level 0=シェル常駐、Level 1+=子プロセス置換 |
| カーネル側規約 | GCC (System V) + `__cdecl` ラッパー |
| 外部プログラム規約 | System V i386 ABI (スタック渡し) |
| 現在のAPIバージョン | **26** |
| プログラム専用スタック | 動的配置 (128KB, メモリ終端付近から下方に展開) |
| スタック保護 | GUARD B (Not-Present) ガードページによる保護 |

### 実行方式

> [!WARNING]
> **ヒープメモリの静的割り当てに関する注意**
> OS32Xバイナリ（外部プログラム）のヒープ領域（`kmalloc` / `mem_alloc` で利用可能なサイズ）は、**ビルド時 (`mkos32x.py` 実行時) に静的に決定**され、バイナリヘッダに書き込まれます。カーネルはプログラムロード時にそのサイズ分のページを固定で割り当てるため、**実行時の動的なヒープ拡張はできません**。
> 大容量のファイル展開やバッファなどを確保するプログラムを作成する場合は、必ず `Makefile` の `mkos32x.py` 呼び出し部分にて十分な `--heap` サイズを指定してください。メモリが不足すると `ENOMEM` エラーを引き起こします。

```bash
> ./program.bin     # 直接実行
> program           # 未知コマンド → 自動的に *.bin を補完探索
> exec program.bin  # 明示的exec
```

### 主な外部化済みプログラム

### 実行ステータスコード (`exec_status_t`)

`exec_run` 関数は実行結果として、`exec_status_t` 列挙型の値を返します (定義: `os32_kapi_shared.h`)。
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
| shell | `programs/shell/` | システム標準シェル (階層化モジュール構造、スクリプトエンジン内蔵) |
| edit | `programs/edit/` | OS32 Edit (VZ Editorにインスパイアされたテキストエディタ) |
| skk_test| `programs/skk/` | SKKフロントエンド (廃止予定) |
| fep_test| `programs/fep_test.c` | FEP (日本語入力) テスト (ベータ) |
| gfx_demo| `programs/gfx_demo.c` | libos32gfx グラフィックスデモ |
| spr_test| `programs/spr_test.c` | スプライト描画テスト |
| demo1 | `programs/demo1.c` | ランス画像表示デモ (VDP/スプライト) |
| raster | `programs/raster.c` | ラスタパレット効果デモ |
| hrview | `programs/hrview.c` | 高解像度画像ビューア |
| vdpview | `programs/vdpview.c` | VDP画像ビューア |
| vbzview | `programs/vbzview.c` | VBZベクタ画像ビューア |
| ekakiuta| `programs/ekakiuta.c` | えかきうたアニメーション |
| bench | `programs/bench/` | ベンチマークプログラム |
| install | `programs/install.c` | HDDインストーラ |
| grep | `programs/grep.c` | 行フィルタ (部分文字列マッチ、ファイル/stdin両対応) |
| wc | `programs/wc.c` | 行/単語/バイトカウント (-l/-w/-c) |
| head | `programs/head.c` | 先頭N行表示 (デフォルト10行) |
| tail | `programs/tail.c` | 末尾N行表示 (デフォルト10行) |
| tee | `programs/tee.c` | stdinをstdout+ファイルに分岐出力 |
| man | `programs/man.c` | マニュアルページビューア |
| more | `programs/more.c` | ページャ (ページ送り/検索/逆スクロール) |
| mdview | `programs/mdview.c` | Markdown簡易ビューア |
| sleep | `programs/sleep.c` | 指定秒数のウェイト (PIT 100Hz) |
| touch | `programs/touch.c` | 空ファイル作成 |
| hexdump | `programs/hexdump.c` | 16進+ASCIIダンプ表示 (-nオプション) |
| find | `programs/find.c` | ファイル名検索 (再帰走査、-name部分一致) |
| sort | `programs/sort.c` | 行ソート (シェルソート、-r/-nオプション) |
| du | `programs/du.c` | ディスク使用量表示 (再帰合算、-sサマリー) |
| cal | `programs/cal.c` | カレンダー表示 (ツェラーの公式、RTC連携) |
| diff | `programs/diff.c` | 簡易2ファイル比較 (行単位逐次比較) |
| hello | `programs/hello.c` | Hello World テスト |
| args | `programs/args.c` | コマンドライン引数表示テスト |
| crash | `programs/crash.c` | 意図的例外発生テスト |
| nop | `programs/nop.c` | 何もしないプログラム |
| restest | `programs/restest.c` | リソーステスト |
| stat_t | `programs/stat_t.c` | stat API テスト |
| libc_test| `programs/libc_test.c` | newlib libc動作テスト |
| test2-4 | `programs/` | APIテスト・システム検証用 |
