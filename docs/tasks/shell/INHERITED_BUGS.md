# 常駐シェルの継承バグ台帳 (T9 の Codex 網羅レビューで顕在化、2026-09-13)

T9 (sh.bin = 常駐シェルの CPL=3 ビルド) の実装レビュー (Codex、往復 6〜9) で、**T9 以前から常駐シェルにもあった**欠陥が
まとめて挙がった。T9 の判定は「T9 由来 + T9 の修正が持ち込んだ回帰」で行い、ここに載るものは別作業で扱う。
到達可能な反例つきのものは T9 の往復の中で直した (S7 の例外として常駐 .o が変わる)。残りは未修正。

## T9 の往復で修正済み (常駐にも効く別コミット)

| ID | 内容 | コミット |
|---|---|---|
| R6 | 別 FS への `mv` がコピー失敗後も原本を削除 (`do_copy_file` が 0 を返していた) | `20ea335` |
| R7 | 256 引数で `argv[256]` の配列外書き込み | `940b5d6` |
| I1 | 引数上限で切り捨てて `cp` の宛先が変わる → 行を捨てる | `6aed6d2` |
| I2 | HostDrv (`st_ino` 0) で `cp /host/a /host/./a` が原本を切り詰める → パス正規化 | `6aed6d2` |
| I3 | `dd cd0` が 1024B 確保で 2048B 受ける | `6aed6d2` |
| I4 | `wildcard_match` の指数的バックトラック → 反復型 (往復 9 で C-1 回帰、修正中) | `6aed6d2` |
| I-1〜I-6 | `ide` の `IdeInfo` 不一致 / `env_expand` の打ち切り / `fs_join_path` の切り詰め / `cp -r` の 31B 名 / `dd hd0` のセクタ長 / `dd` の write 戻り値 | `ecaba37` |
| C-1 | I4 の反復型 `wildcard_match` が `*` を含む名前を落とす回帰 | `0c44364` |

## 未修正 (non-blocker、別作業)

- 注入リング満杯後に次のキーも失う (`ui.c` の UTF-8 後続待ち)。
- 内蔵 `cat` は stdin を読まない (`echo a | cat` / `cat < file` は空)。
- 複合内蔵 (`source file > out`) のリダイレクトが内側の `execute_command` で解除される。
- 255B 超の起動要求が「GUI が必要」と誤表示 (`sh_launch.inc`)。
- `source` が入力の先頭を捨てる (`cmd_script.c:191` が ESC 以外も消費)。
- `ask` が ASCII しか格納しない (日本語を捨てる)。
- 入れ子 glob (`source /tmp/s*.sh` の内側で glob) の解放一覧が共有され解放漏れ / 二重 free 診断。
- 引用付き builtin (`"echo" a | echo b`) をパイプ事前判定が誤拒否。引用内の `|` も分割。`exec` は空白入り引数の引用を復元しない。
- `cp -r` の収集表 64 件上限で無言に切る。
- symlink の型が DIR 以外 FILE (`ext2_vfs.c:59`)。
- glob の `mem_alloc` 失敗が行の失敗に伝わらない (`sh_args.inc:79`)。
- `?` は照合では 1 バイトに一致するが glob の開始条件は `*` だけ。
- `sys_ls` コールバック内の KAPI 再入 (CPL=0 で int 0x80) — **カーネル側**。`find.bin` が CPL=3 で `[Process crashed]`。sh.bin は写し取りで回避 (T9 B2)。
- カーネル帯ポインタを CPL=3 に返す KAPI 5 本 (`vfs_devname` / `path_get_drive` / `path_get_cwd` / `db_column_text` / `db_last_error`) — **カーネル側**。`sys_getcwd` は T9 R1 で `ring3_user_str` の写しに。
- `losetup` 後の退場で loop スロットが回収済み FD を保持 (`drivers/loop_dev.c:928`) — **カーネル側**。sh.bin では `losetup` / `dd loN` を cui only に。
- `source` の 255B 超の行が切断されて実行される (`cmd_script.c:121`、`env_expand` の検知より前)。
- ファイル名補完が 126B で名前を切る (`ui.c:210`)。
- `cat -n` が読み込み区切りを行末として扱い余分な番号付き空行を出す (`cmd_file.c:353`)。
