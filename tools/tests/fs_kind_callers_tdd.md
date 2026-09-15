# TASK_FS_TYPE §3 — 種別が「分からない」とき cp / mv / rm が断る (ホスト試験の記録)

- 票: [`docs/tasks/shell/TASK_FS_TYPE.md`](../../docs/tasks/shell/TASK_FS_TYPE.md) §3
  (`fs_is_dir` は「不明」を運べない)
- 原則: [`docs/POLICY_DEBUG.md`](../../docs/POLICY_DEBUG.md) §4-35 —
  「読めなかった」を「無い / その型ではない」と読み替えない。判定関数ではなく**受け手**を試す
- 実行: `python3 -B tools/tests/test_fs_kind_callers.py [--target]`
  (`make check-fs-kind-callers-host` が同じものを `--target` 付きで回す)
- 対象: `userland/shell/cmd_file.c` の呼び出し元 5 箇所 (`cmd_fs_shared.c` と一緒に実物を `#include`)
- 贋 FS: [`fs_kind_fake.h`](fs_kind_fake.h) — [`fs_kind_host.c`](fs_kind_host.c) から切り出して共有
  (切り出しの前後で `fs_kind_host` は 21 checks, 0 failures のまま)

## 1. 何を確かめる試験か

`fs_is_dir()` は `fs_path_kind() == FS_KIND_DIR` なので、stat と列挙の**両方**が
失敗すると 0 を返す。`cmd_file.c` の 5 箇所はその 0 を「ファイル」と読んでいた。

| 箇所 | 以前の読み替え | 起きること |
|---|---|---|
| `cmd_cp` の宛先 | 読めないディレクトリ = ファイル | `cp -r /src /u` が `/u` 直下へ展開し **`/u/a.txt` を上書き** |
| `cmd_cp` の入力 | 読めない入力 = ファイル | ファイルとして開いて写す |
| `do_move_one` (rename が `INVAL`) | 読めない入力 = ファイル | コピー + `unlink` へ進む |
| `cmd_mv` の宛先 | 読めないディレクトリ = ファイル | 宛先名そのものへ `rename` |
| `cmd_rm` | 読めない対象 = ファイル | **読めないディレクトリを `unlink` へ渡す** |

注入は `sys_stat` と `sys_ls` の両方に `OS32_ERR_IO`。断ったことは

- 断りの文言 `cannot determine the type of` が出る
- `open` / `mkdir` / `rename` / `unlink` の呼び出しが 0 回で、贋 FS の中身が変わらない

の 2 つで見る。`NOTFOUND` は「無いと分かった」なので断りにしない (従来の
`cannot open` / `cannot remove` / 無い名前への `cp` / `mv`)。その退行も同じ試験で見る。

シェルのコマンド関数は `void (*)(int, char **)` で終了状態を持たないので、
「失敗の戻り値」は**その場で打ち切る** (宛先が分からなければコマンド全体、入力 1 件が
分からなければその 1 件を飛ばす — 既存の `path too long` と同じ扱い) で表す。

## 2. RED (修正前の `cmd_file.c`、2026-09-16)

```
HOST GNU89 -Werror COMPILE PASS (real cmd_fs_shared.c + cmd_file.c)
=== cp / mv / rm は種別が分からないと断る (TASK_FS_TYPE §3) ===
== 前提: 注入で fs_path_kind が負 (判定できない) になる ==
  ok   ディレクトリの stat+列挙 IO は IO
  ok   ファイルの stat IO は IO
== 1. cp の宛先が分からない ==
  FAIL cp -r /src /u: 断りを出す
  FAIL cp -r /src /u: **/u/a.txt を上書きしない** (宛先をディレクトリでないと読んで /u 直下へ展開しない)
  FAIL cp -r /src /u: open/mkdir を呼ばない
  FAIL cp /a.txt /u: 断りを出す
  FAIL cp /a.txt /u: 宛先を O_CREAT で開かない
  FAIL cp 複数 → /u: 断りを出す
  FAIL cp 複数 → /u: 「ディレクトリでない」と言い切らない
  ok   cp 複数 → /u: 何も開かない
== 2. cp の入力が分からない ==
  FAIL cp -r /su /dst: 断りを出す
  FAIL cp -r /su /dst: ファイルとして開かない
  ok   cp -r /su /dst: /dst/su を作らない
  FAIL cp /uf /ok.txt /dst: /uf は断る
  FAIL cp /uf ...: /dst/uf を作らない
  ok   cp /uf /ok.txt /dst: 分かる入力 /ok.txt は写す (1 件の断りで全体を止めない)
== 3. mv が FS をまたぐとき、入力が分からない ==
  ok   mv /su /dst/x: まず rename を試す
  FAIL mv /su /dst/x: rename が INVAL なら断りを出す
  FAIL mv /su /dst/x: コピーも unlink もしない
  ok   mv /su /dst/x: 原本が残り、宛先を作らない
== 4. mv の宛先が分からない ==
  FAIL mv /a.txt /u: 断りを出す
  FAIL mv /a.txt /u: /u へ rename しない
  FAIL mv /a.txt /u: 原本が残る
  FAIL mv 複数 → /u: 断りを出す
  FAIL mv 複数 → /u: 「ディレクトリでない」と言い切らない
  ok   mv 複数 → /u: 何もしない
== 5. rm の対象が分からない ==
  FAIL rm /ud: 断りを出す
  FAIL rm /ud: **ディレクトリを unlink しない**
  FAIL rm /uf: 断りを出す
  FAIL rm /uf: unlink しない
== 分かるときは従来どおり ==
  ok   cp ファイル → ディレクトリ
  ok   cp ファイル → 無い名前 (NOTFOUND は「無い」と分かっている)
  ok   cp 無い入力: 断りではなく従来の cannot open
  ok   mv ファイル → ディレクトリ (rename)
  ok   mv ファイル → 無い名前
  ok   mv FS またぎのファイル: コピー + unlink
  ok   mv FS またぎのディレクトリ: 従来の断り (種別は分かっている)
  ok   rm ファイル
  ok   rm ディレクトリ: 従来の断り
  ok   rm 無いもの: 従来の cannot remove

40 checks, 22 failures
EXIT fs_kind_callers_host=1
TARGET i386-elf -Werror COMPILE PASS (userland/shell/cmd_fs_shared.c)
TARGET i386-elf -Werror COMPILE PASS (userland/shell/cmd_file.c)
```

RED のうち `ok` になっている「何も開かない」「/dst/su を作らない」「原本が残る」は、
贋 FS の `open` がディレクトリを `ISDIR` で断るので結果的に無害だっただけで、
分岐は誤っている (同じ節の「断りを出す」が FAIL)。

## 3. 修正

`cmd_file.c` に `file_kind_or_refuse(cmd_name, path)` を置き、5 箇所の `fs_is_dir()` を
置き換えた。戻り値は `FS_KIND_DIR` / `FS_KIND_FILE` / `OS32_ERR_NOTFOUND` (無いと分かった)、
それ以外の負値は断りを表示済み (`<cmd>: cannot determine the type of '<path>': <strerror>`)。

- `cp` / `mv` の宛先が分からない → コマンド全体を打ち切る
- `cp` の入力が分からない → その 1 件を飛ばす
- `mv` の FS またぎで入力が分からない (または無い) → コピー + `unlink` へ進まない
- `rm` の対象が分からない → `unlink` を呼ばない

`fs_is_dir()` 自体は変えていない (`fs_kind_host.c` が「判定できないときの真偽は 0」を
押さえている)。シェル内の呼び出し元はこれで無くなった。

## 4. GREEN

```
HOST GNU89 -Werror COMPILE PASS (real cmd_fs_shared.c + cmd_file.c)
=== cp / mv / rm は種別が分からないと断る (TASK_FS_TYPE §3) ===
== 前提: 注入で fs_path_kind が負 (判定できない) になる ==
== 1. cp の宛先が分からない ==
== 2. cp の入力が分からない ==
== 3. mv が FS をまたぐとき、入力が分からない ==
== 4. mv の宛先が分からない ==
== 5. rm の対象が分からない ==
== 分かるときは従来どおり ==

40 checks, 0 failures
EXIT fs_kind_callers_host=0
TARGET i386-elf -Werror COMPILE PASS (userland/shell/cmd_fs_shared.c)
TARGET i386-elf -Werror COMPILE PASS (userland/shell/cmd_file.c)
```

(`ok` の行は省いた。40 件すべて `ok`。) 同じ変更で `test_fs_kind.py --target` は
21 checks, 0 failures、`test_sh_shell.py --target` は ALL PASS のまま。実行時間 約 0.4 秒。

## 5. この試験が言わないこと

- **実機の `cp` / `mv` / `rm` は見ていない。** 贋 FS はオンメモリで、実物の VFS も ext2 も通らない。
- 「分からない」の作り方は `stat` + 列挙の `IO` だけ。`NOSYS` など他の負値は同じ分岐を通るが個別には試していない。
- `cp -r` の再帰の内側 (`do_copy_recursive_impl`) は列挙の `type` を使うので `fs_path_kind` を呼ばず、この試験の対象外。
