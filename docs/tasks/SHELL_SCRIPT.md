# OS32 簡易シェルスクリプト機能 (バッチ処理) 仕様書

## 1. 概要

OS32の外部シェル（`programs/shell.bin`）において、テキストファイルに記述されたコマンド群を
順次実行する「バッチ処理機能」を実装する。

複雑な構文解析（`while` や `{}` ブロック等）は避け、既存の `execute_command()` と
環境変数展開（`$VAR`）を最大限に活用し、軽量かつDOSライクな `if` + `goto` の
組み合わせによる制御構文を提供する。

## 2. コメント仕様

| 種別 | 構文 | 説明 |
|------|------|------|
| 単行コメント | `# ...` | 行の先頭が `#` で始まる行 |
| 単行コメント | `// ...` | 行の先頭が `//` で始まる行 |
| 複数行コメント | `'''` ... `'''` | `'''` のみの行で囲まれたブロック全体（改行を含む） |

`'''` ブロックコメントはロードフェーズ（オンメモリ配列構築時）でフィルタリングする。
実行フェーズでは既にコメント行が除去されるため、`goto` のラベル検索も高速化される。

## 3. 追加コマンド仕様

シェル内部コマンドとして以下の5つを新規実装する。
全て `cmd_script.c` に配置する。

### 3-1. `source` コマンド (スクリプト実行)

指定されたテキストファイルを読み込み、1行ずつパースして実行する。

* **書式:** `source FILE`
* **機能:**
  * ファイルを行単位で読み込み、オンメモリ配列に格納して実行する。
  * `AUTOEXEC.BAT` や `.profile` などの実行の起点となる。
  * ネスト深度は `SCRIPT_MAX_DEPTH` (4) まで。超過時はエラー表示。
  * `.bat` または `.sh` 拡張子のファイルが直接入力された場合、
    `run_cmd_internal` で暗黙的に `source` として実行する。
* **コメント:** `#`, `//` で始まる行および `'''` ブロックはスキップ。
* **空行:** 空行もスキップ。

### 3-2. `ask` コマンド (ユーザー入力と変数代入)

ユーザーからのキー入力を受け付け、その結果を環境変数に格納する。

* **書式:** `ask PROMPT_WORDS... VAR_NAME`
* **機能:**
  * **最後の引数**を変数名、それ以前の全引数を結合してプロンプト文字列とする。
  * 引数中の二重引用符 `"` は装飾として除去する。
  * 画面にプロンプトを表示し、Enterキーが押されるまでキー入力をバッファリングする。
  * 入力された文字列を `VAR_NAME` として `env_set()` で環境変数にセットする。
* **使用例:** `ask "Continue? [y/n] " ANS`
  * → 画面に `Continue? [y/n] ` と表示、入力値が `$ANS` に格納される。

### 3-3. `if` コマンド (1行条件分岐)

ブロックを持たない、1行完結型の条件分岐を実行する。

* **書式:**
  * `if VAL1 == VAL2 COMMAND...` — 文字列一致で真
  * `if VAL1 != VAL2 COMMAND...` — 文字列不一致で真
  * `if exist PATH COMMAND...` — ファイル/ディレクトリが存在すれば真
  * `if not exist PATH COMMAND...` — 存在しなければ真
* **機能:**
  * 条件が真の場合のみ、残りの引数を結合して `execute_command()` に渡す。
  * `$VAR` 展開は `execute_command()` 内の `env_expand()` により自動的に行われる
    ため、`if` コマンド到達時点では既に展開済みとなる。
* **使用例:**
  * `if "$ANS" == "y" echo "OK!"`
  * `if exist /hd1/bin/app.bin echo "Found!"`
  * `if "$?" == "0" echo "Previous command succeeded"`

### 3-4. `goto` コマンドとラベル (ジャンプ)

スクリプト内の特定行へ実行ポインタを移動させ、ループや複雑な分岐を実現する。

* **書式:** `goto LABEL`
* **ラベル定義:** 行の先頭が `:` で始まる文字列（例: `:START`）。
  ラベル行自体は実行時にスキップされる。
* **機能:**
  * スクリプトの `current_line` を対象ラベルの位置に変更する。
  * ラベルが見つからない場合はエラー表示してスクリプトを中断する。

### 3-5. `return` コマンド (スクリプト終了)

現在のスクリプト実行を終了し、呼び出し元に戻る。

* **書式:** `return`
* **機能:**
  * `current_line` を `line_count` にセットして実行ループを終了する。
  * ネストされた `source` の場合は親スクリプトに制御が戻る。
  * 対話シェルから直接実行された場合は何もしない（無害）。

## 4. 特殊変数

| 変数 | 内容 |
|------|------|
| `$?` | 直前に実行したコマンドの終了ステータス (`0`=成功) |

`execute_command()` の実行後、戻り値を `env_set("?", ...)` で自動的にセットする。

## 5. スクリプト実行エンジンのアーキテクチャ設計

`goto` による前方・後方へのジャンプを実現するため、`source` コマンドは
**オンメモリ配列方式**を採用する。

### 5-1. 定数・構造体

```c
#define SCRIPT_MAX_LINES  256   /* 最大行数 */
#define SCRIPT_MAX_LINE   256   /* 1行の最大長 */
#define SCRIPT_MAX_DEPTH  4     /* source ネスト上限 */

typedef struct {
    char   lines[SCRIPT_MAX_LINES][SCRIPT_MAX_LINE];
    int    line_count;       /* ロードされた行数 */
    int    current_line;     /* 実行中の行インデックス */
    int    abort_flag;       /* ESCキーで中断 */
} ScriptContext;
```

静的バッファ (BSS) を使用する。`mem_alloc` は使わない。
合計約 65KB (256 * 256 + ポインタ配列)。

### 5-2. ロードフェーズ

1. `sys_read()` を用いてスクリプトファイル全体をメモリ上に読み込む。
2. 改行 (`\n`) で区切って `lines[][]` 配列に格納する。
3. **この段階で** `#`, `//` コメント行、空行、`'''` ブロックを除外する。
4. ラベル行 (`:LABEL`) はそのまま保持する（`goto` の探索対象として必要）。

### 5-3. 実行フェーズ

1. `current_line = 0` から開始。
2. 各行の先頭が `:` ならラベルとしてスキップ（`current_line++`）。
3. それ以外の行は `execute_command()` に渡す。
4. **各行実行前にESCキーをポーリング** — 押されたらスクリプト中断。
5. `current_line++` でインクリメントし、`line_count` に達したら終了。

### 5-4. ジャンプ処理 (`goto`)

* `goto LABEL` が呼ばれたら、`lines[]` の先頭から `:LABEL` と一致する行を探索する。
* 発見した場合、`current_line` をその行番号 +1 に書き換える。
* 未発見の場合はエラー表示してスクリプトを中断する。

### 5-5. ESCキーブレーク

```c
/* 各行実行前のブレークチェック */
if (g_api->kbd_iskey()) {
    int k = g_api->kbd_getchar();
    if ((k & 0xFF) == 0x1B) {       /* ESC */
        g_api->kprintf(ATTR_RED, "^C Script aborted.\n");
        ctx->abort_flag = 1;
        break;
    }
}
```

## 6. 拡張子自動認識

`run_cmd_internal()` の内部コマンド検索の前に、拡張子による自動判定を追加する。

```c
/* .bat / .sh 拡張子は source として実行 */
if (has_bat_ext(argv[0]) || has_sh_ext(argv[0])) {
    cmd_source(argc, argv);
    return;
}
```

## 7. スクリプト実装例 (`AUTOEXEC.BAT`)

```text
# OS32 初期化スクリプト
echo "Starting OS32 System..."
mount /hd1/ hd1 ext2

:MENU
echo "=== OS32 MAIN MENU ==="
echo "1. Show System Info"
echo "2. Start Remote Shell"
echo "3. Exit to Prompt"

ask "Select Task [1-3]: " TASK

if "$TASK" == "1" uname
if "$TASK" == "1" mem
if "$TASK" == "2" serial
if "$TASK" == "2" rshell
if "$TASK" == "3" goto END

// 無効な入力の場合はループ
echo "Invalid choice."
goto MENU

:END
echo "Welcome to OS32."
```

複数行コメントの例:

```text
'''
このスクリプトは OS32 の起動時に自動実行される。
HDDのマウントと初期メニュー表示を行う。
'''

echo "System initialized."
```

## 8. 変更対象ファイル

| ファイル | 変更内容 |
|----------|----------|
| `programs/shell/cmd_script.c` | **新規**: source, if, goto, ask, return コマンド実装 |
| `programs/shell/shell.h` | ScriptContext 構造体、定数、プロトタイプ追加 |
| `programs/shell/main.c` | shell_cmd_script_init() 呼び出し、.bat拡張子判定追加 |
| `programs/shell/ui.c` | .profile ロードを source 呼び出しに置換 |
| `Makefile` | cmd_script.o をシェルのオブジェクトリストに追加 |

## 9. 開発フェーズ (マイルストーン)

### Phase 1: スクリプトエンジン基盤

* `cmd_script.c` を新規作成
* `source` コマンド（逐次実行版）を実装
* `ui.c` の `.profile` ロードロジックを `source` 呼び出しに置換
* コメント処理 (`#`, `//`, `'''`) を実装
* Makefile に `cmd_script.o` を追加

### Phase 2: 条件分岐と入力

* `ask` コマンドの実装（引数結合方式、引用符除去）
* `if` コマンドの実装（`==`, `!=`, `exist`, `not exist`）
* 特殊変数 `$?` の実装（`execute_command` 終了ステータス）

### Phase 3: ジャンプ機能

* `source` をオンメモリ配列方式へ改修 (ScriptContext)
* `goto` コマンド + ラベル解析を実装
* `return` コマンドを実装
* ESCキーブレーク機構を実装
* ネスト深度制限の実装 (`SCRIPT_MAX_DEPTH`)

### Phase 4: システム統合

* `run_cmd_internal()` に `.bat`/`.sh` 拡張子自動認識を追加
* `main.c` のエントリポイントで `AUTOEXEC.BAT` 自動実行処理を追加
* manページ (`source.1`, `if.1`, `goto.1`, `ask.1`) を作成
