## 第7部 シェル (外部プログラム)

OS32カーネルは内蔵シェルを持たず、起動時に外部プログラム `userland/shell.bin` を実行してシステム制御を引き渡します。各種コマンドや入力機能はすべてこのシェルプログラムがKernelAPIを介して提供します。

### §7-1 コマンド一覧

**基本コマンド** (cmd_base.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `help` / `?` | `help [cmd]` | コマンド一覧表示 |
| `clear` / `cls` | `clear` | 画面クリア |
| `tick` | `tick` | タイマカウンタ表示 |
| `ver` / `uname` | `ver` | OS バージョン表示 |
| `date` | `date` | RTC日時表示 |
| `beep` | `beep` | 起動ジングル再生 |
| `uptime` | `uptime` | 稼働時間表示 |
| `np2` | `np2` | NP21/W エミュレータ検出 |
| `time` | `time CMD` | コマンドの実行時間を計測 |

**システムコマンド** (cmd_sys.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `mem` / `heap`| `mem` | メモリ情報とヒープ使用状況 |
| `reboot` | `reboot` | システム再起動 |
| `dev` / `df` | `dev` | ブロック・キャラクタデバイス一覧 |
| `ide` | `ide [0-3]` | IDEドライブのCH/S・LBA情報 |
| `format` | `format [0-3] [sects]` | ドライブをext2でフォーマット |
| `play` | `play MML` | MML文字列をFM音源で再生 |
| `os32gui` | `os32gui [on\|off]` | GUI シェル (/bin/gshell.bin) へ切り替え / 起動時 GUI の既定を `/etc/system.cfg` に書く (GUI v1.1 K4) |
| `gfxmode` | `gfxmode pc98\|pegc\|cirrus\|auto` | 次回起動のグラフィクスバックエンドを `/etc/system.cfg` の `GFX=` に書く (GUI v1.1 H2b) |

**ディレクトリコマンド** (cmd_dir.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `ls` | `ls [-la] [path...]` | ディレクトリ一覧表示 (`-a` で `.`/`..` も表示) |
| `cd` | `cd [path]` | カレントディレクトリ変更 (引数なしで `$HOME` へ) |
| `pwd` | `pwd` | カレントディレクトリ表示 |
| `mkdir` | `mkdir dir...` | ディレクトリ作成 |
| `rmdir` | `rmdir dir...` | ディレクトリ削除 |

**ファイルコマンド** (cmd_file.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `cp` | `cp [-r] SRC DST / SRC... DIR` | ファイルのコピー (`-r` で再帰) |
| `mv` | `mv SRC DST / SRC... DIR` | ファイルの移動 |
| `rm` | `rm FILE...` | ファイルの削除 |
| `cat` / `cat2` | `cat [-n] FILE...` | ファイル内容表示 (`-n` で行番号付き) |
| `echo` | `echo [args...] [> FILE]` | テキスト出力 (stdout経由) |

**マウント・実行コマンド** (cmd_mnt.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `mount` | `mount PREFIX DEV FS` | マウント (例: `mount /hd0/ hd0 ext2`) |
| `umount`| `umount PREFIX` | アンマウント |
| `sync` | `sync` | メタデータ書き戻し |
| `exec` | `exec FILE.BIN` | OS32Xバイナリを明示的に実行 |

**環境変数コマンド** (cmd_env.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `env` | `env` | 全環境変数を表示 |
| `set` / `export` | `set VAR=VALUE` | 環境変数を設定 |
| `unset` | `unset VAR...` | 環境変数を削除 |

**リモートシェル(rshell) コマンド** (rshell.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `serial` | `serial` | RS-232C初期化 + SerialFS マウント |
| `terminal` | `terminal` | ターミナルモード (ESCで終了) |
| `rshell` | `rshell` | リモートシェルモード開始 (ESCで終了) |
| `send` | `send TEXT...` | RS-232C文字列送信 |
| `hotdeploy` | `hotdeploy [PATH LEN CRC32]` | ステージング領域の内容をファイル化 (引数なしで領域の番地を報告)。ホスト側は `tools/hotdeploy.py` から使う |
| `recv` | `recv [host:PATH [LOCAL]]` | ファイル受信 (SerialFS または旧プロトコル) |
| `push` | `push LOCAL host:PATH` | SerialFS 経由でホストへファイル送信 |
| `tvdump` | `tvdump` | テキストVRAMダンプをシリアル送信 |

※ 起動時に自動的にrshellモードに入るため対話コマンドもホストと連携可能です。

**ファイラ・ファイル管理コマンド** (cmd_filer.c):

| コマンド | 書式 | 説明 |
|---------|------|------|
| `filer` | `filer [dir]` | CUIファイラ (カーソル移動・Enter実行・拡張子関連付け) |

**その他の組み込みコマンド**:

| コマンド | 書式 | 説明 |
|---------|------|------|
| `losetup` | `losetup <path> <slot> \| -d <slot> \| -l` | ループデバイス管理 (ディスクイメージをブロックデバイスとして接続) |
| `dd` | `dd <dev> lba=N count=M [file=PATH]` | ブロックデバイスのセクタ読み出し |
| `export` | `export VAR=VALUE` | 環境変数の設定 |

**エイリアス**: `cls`→`clear`、`uname`→`ver`、`.`→`source`、`cat2`→`cat`、
`heap`→`mem`、`df`→`dev`。

### §7-2 プログラム実行とPATH探索

| 実行方法 | 例 | 説明 |
|---------|------|------|
| 直接実行 | `./test2.bin` | カレントディレクトリのバイナリを実行 |
| コマンド名実行 | `test2` | 未知コマンド → `.bin` を補完して探索 |
| PATH探索 | `grep hello` | `$PATH` 内のディレクトリを順に探索 |

- 環境変数 `PATH` にコロン区切りでディレクトリを設定可能 (デフォルト: `/bin:/sbin:/usr/bin` — `config.h` の `SYS_DEFAULT_PATH`)
- 起動時に `/etc/profile` → `$HOME/.profile` の順で自動読み込み (環境変数・PATHの初期設定用)
- `*` や `?` などの簡単なワイルドカードもサポート

**外部コマンド** (`/bin` に配置、`userland/cmds/`):
`cal` `diff` `du` `find` `grep` `head` `hexdump` `ime` `less` `man` `more` `sleep` `sort` `tail` `tee` `touch` `v86` `wc`
(`ime` は FEP の有効化/辞書操作、`v86` は V86 モードでのゲスト起動)
(その他 `/sbin` に `install` `cdinst`、`/usr/bin` にアプリ群。詳細は [09_exec.md](09_exec.md) 参照)

### §7-3 パイプ・リダイレクト

シェルは標準入出力のリダイレクトとパイプラインをサポートします。

| 演算子 | 書式 | 説明 |
|--------|------|------|
| `>` | `cmd > file` | stdoutをファイルに書き出し (上書き) |
| `>>` | `cmd >> file` | stdoutをファイルに追記 |
| `<` | `cmd < file` | ファイルをstdinとして入力 |
| `\|` | `cmd1 \| cmd2` | cmd1のstdoutをcmd2のstdinに接続 |

- パイプはカーネルのパイプバッファAPI (`sys_pipe_alloc` 等) を使用し、逐次実行方式で動作
- リダイレクトはカーネルのFDリダイレクトAPI (`sys_redirect_fd` 等) を使用
- フィルタコマンド (grep, wc, head, tail, tee, sort, more, less 等の外部コマンド) はstdin/ファイル両対応で、パイプラインと連携可能
- `ls` / `pwd` は `isatty(1)` で出力先を判定し、非TTY時は1行1エントリ形式で出力

### §7-4 入力機能

| 機能 | キー | 説明 |
|------|------|------|
| コマンド履歴 | ↑/↓ | 16件リングバッファ (`.history` に永続化、起動時に復元) |
| Tab補完 | Tab | 共通プレフィクス補完 (ディレクトリ探索含む) |
| Tab候補表示 | Tab×2 | 候補一覧表示 |
| カーソル移動 | ←/→ | 行内移動 |
| 行頭移動 | Home | カーソルを行頭に |
| 文字削除 | Del | カーソル位置の文字を削除 |
| 行クリア | ESC | 入力中の行をクリア |
| バックスペース | BS | カーソル左の文字を削除 |

**日本語入力**: 入力は `ime_getkey()` (KernelAPI) 経由で取得され、IME/FEP有効時は
UTF-8マルチバイト文字の入力・表示 (`shell_print_utf8`) に対応する。

### §7-5 スクリプトエンジン

シェルはバッチスクリプト実行機能を内蔵しています。起動時に `/etc/profile` が自動実行され、環境変数やPATHの初期設定が行われます。

| コマンド | 書式 | 説明 |
|---------|------|------|
| `source` / `.` | `source FILE` | 指定ファイルをスクリプトとして実行 |
| `if` | `if VAL1 == VAL2 CMD...` | 条件一致時のみ CMD を実行 (ELSE 分岐なし) |
| `goto` | `goto LABEL` | ラベルへのジャンプ |
| `return` | `return` | スクリプト実行を終了してシェルに復帰 |
| `ask` | `ask "prompt" VAR` | ユーザー入力を受け取り環境変数 VAR に格納 |

**ラベル定義**: 行頭に `:LABEL` 形式で記述。`goto LABEL` でジャンプ先を指定。

---
