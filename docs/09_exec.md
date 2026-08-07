## 第9部 外部プログラム実行 (exec)

OS32Xバイナリ形式の実行ファイルをext2から0x400000にロードし、
KernelAPIポインタを引数として実行する。

詳細は **[KAPI_SPEC.md](KAPI_SPEC.md)** を参照。

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| ソースファイル | `exec/exec.c` / `exec/exec.h` / `exec/exec_heap.c` |
| KAPIラッパー | `kapi/kapi_*.c` (自動生成分 + 手動分) |
| KAPIテーブルアドレス | 動的算出 (KHEAP_BASE + KHEAP_SIZE) |
| ロードアドレス | 0x400000 (固定・プロセス間共有) |
| プログラム専用ヒープ | 動的配置 (sbrk_heap_limit, exec_heap 管理下) |
| ネスト実行 | 最大4段 (MAX_EXEC_NEST)。Level 0=シェル常駐、Level 1+=子プロセス置換 |
| カーネル側規約 | GCC (System V) + `__cdecl` ラッパー |
| 外部プログラム規約 | System V i386 ABI (スタック渡し) |
| 現在のAPIバージョン | **31** |
| プログラム専用スタック | 動的配置 (256KB, メモリ終端付近から下方に展開) |
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

### 主な外部化済みプログラム

**シェル**:

| プログラム | ソース | 説明 |
|---------|---------|------|
| shell | `programs/shell/` | システム標準シェル (階層化モジュール構造、スクリプトエンジン・ファイラ内蔵) |

**アプリケーション** (`programs/apps/`):

| プログラム | ソース | 説明 |
|---------|---------|------|
| edit | `apps/edit/` | OS32 Edit (VZ Editorにインスパイアされたテキストエディタ) |
| mdview | `apps/mdview.c` | Markdown簡易ビューア |
| vdpview | `apps/vdpview.c` | VDP/高解像度画像ビューア (旧hrviewを統合) |
| vbzview | `apps/vbzview.c` | VBZベクタ画像ビューア |
| demo1 | `apps/demo1.c` | ランス画像表示デモ (VDP/スプライト) |
| ekakiuta| `apps/ekakiuta.c` | えかきうたアニメーション |
| gfx_demo| `apps/gfx_demo.c` | libos32gfx グラフィックスデモ |
| raster | `apps/raster.c` | ラスタパレット効果デモ |
| spr_test| `apps/spr_test.c` | スプライト描画テスト |

**コマンドラインツール** (`programs/cmds/`):

| プログラム | ソース | 説明 |
|---------|---------|------|
| grep | `cmds/grep.c` | 行フィルタ (部分文字列マッチ、ファイル/stdin両対応) |
| less | `cmds/less.c` | ページャ (上下スクロール、検索) |
| more | `cmds/more.c` | ページャ (ページ送り/検索/逆スクロール) |
| man | `cmds/man.c` | マニュアルページビューア |
| wc | `cmds/wc.c` | 行/単語/バイトカウント (-l/-w/-c) |
| head | `cmds/head.c` | 先頭N行表示 (デフォルト10行) |
| tail | `cmds/tail.c` | 末尾N行表示 (デフォルト10行) |
| tee | `cmds/tee.c` | stdinをstdout+ファイルに分岐出力 |
| sort | `cmds/sort.c` | 行ソート (シェルソート、-r/-nオプション) |
| find | `cmds/find.c` | ファイル名検索 (再帰走査、-name部分一致) |
| diff | `cmds/diff.c` | 簡易2ファイル比較 (行単位逐次比較) |
| du | `cmds/du.c` | ディスク使用量表示 (再帰合算、-sサマリー) |
| cal | `cmds/cal.c` | カレンダー表示 (ツェラーの公式、RTC連携) |
| hexdump | `cmds/hexdump.c` | 16進+ASCIIダンプ表示 (-nオプション) |
| sleep | `cmds/sleep.c` | 指定秒数のウェイト (PIT 100Hz) |
| touch | `cmds/touch.c` | 空ファイル作成 |

**システムユーティリティ** (`programs/system/`):

| プログラム | ソース | 説明 |
|---------|---------|------|
| hsync | `system/hsync.c` | HostDrv同期 (/host → / にファイル同期) |
| install | `system/install.c` | HDDインストーラ |
| cdinst | `system/cdinst.c` | CDインストーラ (ISO→ext2展開) |
| sndctl | `system/sndctl.c` | サウンド制御ユーティリティ |
| sndtest | `system/sndtest.c` | FM音源テスト |
| sndtest2| `system/sndtest2.c` | SSG音源テスト |

**テスト・デモ** (`programs/tests/`):

| プログラム | ソース | 説明 |
|---------|---------|------|
| bench | `tests/bench/` | ベンチマークプログラム |
| bench_scale2x | `tests/bench_scale2x/` | Scale2x ベンチマーク |
| sqlite_standalone | `tests/sqlite_standalone/` | SQLite スタンドアロンテスト |
| mouse_test | `tests/mouse_test.c` | マウスドライバテスト |
| flip400_test | `tests/flip400_test.c` | 400ラインページフリップテスト |
| gfx200_test | `tests/gfx200_test.c` | 200ラインGFXテスト |
| gfx_demo200 | `tests/gfx_demo200.c` | 200ライングラフィックスデモ |
| fep_test| `tests/fep_test.c` | FEP (日本語入力) テスト |
| hello | `tests/hello.c` | Hello World テスト |
| args | `tests/args.c` | コマンドライン引数表示テスト |
| crash | `tests/crash.c` | 意図的例外発生テスト |
| nop | `tests/nop.c` | 何もしないプログラム |
| restest | `tests/restest.c` | リソーステスト |
| stat_t | `tests/stat_t.c` | stat API テスト |
| libc_test| `tests/libc_test.c` | newlib libc動作テスト |
| klibc_test | `tests/klibc_test.c` | カーネルlibc動作テスト |
| test2-4 | `tests/` | APIテスト・システム検証用 |
| (ライブラリ別テスト) | `tests/*.c` | ai_test, asset_test/demo, blit_test/2, board_test, btl_test, chem_test/demo, db_test, demo_tile, e2test, econ_test, ecs_test/demo, evt_test, input_test, inv_test, map_test/demo, math_test, rotate_test, text_test/demo, tile_bench |

**Rust プログラム** (`programs/rust/`, カスタムターゲット `i686-os32-none.json` + build-std):

| プログラム | ソース | 説明 |
|---------|---------|------|
| hello_gfx | `rust/hello_gfx/` | Rust グラフィックスデモ |
| alloc_demo | `rust/alloc_demo/` | Rust アロケータデモ |
| math_test_rs | `rust/math_test_rs/` | os32_math クレートのテスト |
| (クレート) | `rust/os32api/`, `rust/os32_math/` | KernelAPI バインディング / 数学ライブラリ |

**ライブラリ**:

| ライブラリ | ソース | 説明 |
|---------|---------|------|
| libos32 | `programs/libos32/` | newlib-nano ブリッジ (syscalls.c) |
| libos32gfx | `programs/libos32gfx/` | グラフィックス描画ライブラリ |
| libos32snd | `programs/libos32snd/` | サウンドライブラリ (BGM/SE) |
| libos32math | `programs/libos32math/` | 整数数学ライブラリ (Q16.16固定小数点) |
| libos32input | `programs/libos32input/` | 入力抽象化ライブラリ |
| libos32db | `programs/libos32db/` | SQLite (db_* KAPI) ラッパー |
| libos32text | `programs/libos32text/` | テキスト/メッセージ管理 |
| libos32asset | `programs/libos32asset/` | アセット管理 |
| libos32ecs | `programs/libos32ecs/` | ECS (Entity Component System) |
| libos32event | `programs/libos32event/` | イベントシステム |
| libos32map | `programs/libos32map/` | マップ/タイル管理 |
| libos32ai | `programs/libos32ai/` | ゲームAI |
| libos32battle | `programs/libos32battle/` | 戦闘システム |
| libos32board | `programs/libos32board/` | ボード(掲示板)システム |
| libos32chem | `programs/libos32chem/` | 化学シミュレーション |
| libos32econ | `programs/libos32econ/` | 経済シミュレーション |
| libos32inv | `programs/libos32inv/` | インベントリ管理 |
| libos32tilemap | `programs/libos32tilemap/` | タイルマップ描画 |
| libos32filer | `programs/libos32filer/` | ファイラ共通ライブラリ |
| libos32md | `programs/libos32md/` | Markdown パーサーライブラリ |
| libos32turn | `programs/libos32turn/` | 手番/週スケジューラ |
| libos32rpg | `programs/libos32rpg/` | キャラクター育成・状態・リボーン |
| libos32save | `programs/libos32save/` | セーブデータ管理 |
