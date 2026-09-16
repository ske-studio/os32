# hsync H2 TDD 記録 — 一時ファイル + 検証 + 置換

対象票: [`docs/tasks/shell/TASK_H2.md`](../../docs/tasks/shell/TASK_H2.md) §2-3 / §2-4 / §4-1
試験: `tools/tests/hsync_h2_host.c` (実物の `userland/system/hsync.c` を `#include`)
実行: `python3 -B tools/tests/test_hsync_h2.py [--target] [--mutate]` / `make check-hsync-h2-host`

基点: `feat/gui` = `15c5edf`。

## 何を見ているか

H1 / H3 と同じく **`userland/system/hsync.c` を 1 行も写さずそのまま `#include`**
し、`KernelAPI` だけをオンメモリの贋ファイルシステムへ差し替える。贋 FS は票 H2 が
要求する故障を注入できる:

- `sys_open` の **`O_EXCL`** (既存は `EXIST` / 非対応 FS は `NOSYS`)
- n 回目の `sys_write` を任意の番号で落とす (空き不足 = `NOSPC` / `FULL` も)
- `sys_set_mtime` の失敗と、それに伴う **`ROFS` への転落** (ext2 と同じ)
- `sys_rename` の模様: **公開の前**に失敗 / **公開の後**に失敗 (一時名が消えた形と
  残った形) / 公開したか判定できない / 宛先が第三の inode になった / 成功
- `st_nlink` をノードごとに持つ (hardlink 判定と予約名の掃除)
- 指定パスの `sys_ls` を n 回だけ落とす (掃除の列挙だけを止め、手順 2 の
  `EXIST` 経路を必ず通す)
- `api->version` を 52 / 53 に切り替える (古いカーネルの門)

受入表との対応 (§4-1):

| ID | 試験の節 |
|---|---|
| A14a | `case_a14a` (write 失敗 / FULL / NOSPC / 読戻し / 公開前の rename) |
| A14a2 | `case_a14a2` (`RN_FAIL_AFTER`) |
| A14a3 | `case_a14a3` (`set_mtime` 失敗 → ROFS → `STALE` → 次の実行が片づける) |
| A14a4 | `case_a14a2` (`RN_UNKNOWN` / `RN_THIRD_INO`) |
| A14a5 | ext2 側は `tools/tests/b8_open_host.c` の段 C 掃引。hsync 側は A15 で収束を見る |
| A14a6 | `case_a14a2` 末尾 (`-f` で新旧同内容 + 公開前の失敗) |
| A14b / A14b2 / A14c | `case_a14b` |
| A15 / A15b / A15c | `case_a15` |
| A17a / A17b / A17c | `case_a17` |
| R1 / R1b | `case_r1` |
| R3 | `case_r3` |
| R4 | `case_r4` |
| R5 | `case_r5` |
| §2-3 手順 2 の `replace_unsupported` | `case_unsupported` |
| R2 (回帰) | `case_regression` + `test_hsync_h1.py` / `test_hsync_h3.py` / `test_hsync_protect.py` |

## RED (実装前 = `15c5edf` の `userland/system/hsync.c`)

KAPI と VFS / ext2 は新しいまま、**hsync だけを `15c5edf` に戻して**回した結果。

```
150 checks, 80 failures
```

主な内訳 (抜粋):

```
== A14a: 公開の前の失敗 -> 旧宛先の内容・サイズ・mtime が不変 ==
  FAIL write 失敗: 旧宛先の内容・サイズ・mtime・ino が不変
  FAIL 公開の前と明示する
  FAIL reason=no_space
  FAIL 空き不足: 旧宛先のサイズ不変
  FAIL 読戻し失敗: 旧宛先が不変
  FAIL 公開前の rename 失敗で非ゼロ終了
  FAIL reason=replace_failed
== A14a2: 公開の後の失敗 -> 新内容を認め replace_partial ==
  FAIL reason=replace_partial
  FAIL **成功には数えない**
== A14a4: 公開したか判定できない -> ino 照合で決まる ==
  FAIL 宛先の stat が落ちたら replace_unknown
== A14a3: 一時ファイルへの set_mtime 失敗 -> ROFS -> STALE ==
  FAIL **copied は 0** (公開の前の失敗)
  FAIL 宛先は旧内容のまま
  FAIL STALE と表示する
== A14b2: 利用者が .hs~notes を置いている -> 消える (予約名) ==
  FAIL **利用者のファイルでも消える**
  FAIL 集計 cleaned=1
== A17a/b/c ==
  FAIL reason=source_changed / dest_changed / hardlink
== R1: KAPI v52 で --unsafe-overwrite 無し -> kernel_too_old ==
  FAIL reason=kernel_too_old
  FAIL **1 件も書かない**
== R3 / R4 / R5 / replace_unsupported ==
  FAIL 本名は不存在のまま / name_too_long / reason=protected / replace_unsupported
```

**旧コードが何をしていたか**: `copy_verify` が宛先を `O_WRONLY|O_CREAT|O_TRUNC` で
**直接**開いていた。書き込み・検証・mtime のどこで落ちても旧内容は既に切り詰められて
いて戻らない (失敗行に「直接上書きなので旧内容は残らない」と出していた)。
一時ファイル・予約名・置換・古いカーネルの門は存在しない。

## GREEN (実装後)

```
MANIFEST PASS (app.conf hsync=52, HS_MIN_KAPI_H2=53, 予約名 .hs~ は man ページに明記)
HOST GNU89 -Werror COMPILE PASS (tools/tests/hsync_h2_host.c)
=== 票 H2: hsync の置換安全化 (一時ファイル + 検証 + 置換) ===
  (150 件すべて ok)

150 checks, 0 failures
EXIT hsync_h2_host=0
TARGET i386-elf -Werror COMPILE PASS (userland/system/hsync.c)
```

## 変異試験 (`--mutate`、否定側)

```
MUTATE publish_by_size            RED (期待どおり落ちた)
MUTATE fallback_direct_on_nosys   RED (期待どおり落ちた)
MUTATE mtime_failure_ignored      RED (期待どおり落ちた)
MUTATE temp_owned_by_nlink        RED (期待どおり落ちた)
MUTATE clean_owned_by_nlink       RED (期待どおり落ちた)
MUTATE no_kernel_gate             RED (期待どおり落ちた)
MUTATE clean_ignores_protection   RED (期待どおり落ちた)
MUTATE temp_names_synced          RED (期待どおり落ちた)
```

- `publish_by_size` … 公開の判定を **サイズ**に戻した版 (往復 2 所見 3 の否定側)。
  `-f` で新旧が同じ内容だと未公開を「公開された」と誤る。
- `fallback_direct_on_nosys` … `NOSYS` で**直接上書きへ黙って落ちる**版。
- `mtime_failure_ignored` … mtime の失敗を握り潰して置換へ進む版
  (往復 1 所見 4 の否定側)。
- `temp_owned_by_nlink` / `clean_owned_by_nlink` … 予約名の所有を **`st_nlink`**
  で判断する版 (往復 1 所見 2 の否定側)。手順 2 の `EXIST` 経路と §2-4 の掃除の
  両方に当てる。
- `no_kernel_gate` … 古いカーネルの門を外した版 (R1 の否定側)。
- `clean_ignores_protection` … 掃除が保護対象を無視する版 (R5 の否定側)。
- `temp_names_synced` … コピー元の `.hs~` を同期対象にしてしまう版 (R4 の否定側)。

## 設計との違い (PM の確認が要る点)

**`build/app.conf` の `userland/system/hsync` の要求 API 版は 52 のまま**にした。

票 §1 の 7 と §2-3 末尾が「v52 のカーネルの上で新しい hsync を動かす場面は現に
起こる」「既定は `kernel_too_old` で断り、`--unsafe-overwrite` のときだけ直接上書き」
としているが、`exec/exec.c:1276` は `hdr->min_api_ver > KAPI_VERSION` のバイナリを
「invalid OS32X binary」として**起動そのものを拒否する**。53 を宣言すると
`kernel_too_old` も `--unsafe-overwrite` も届かない。

そのため `test_hsync_h3.py` の `check_kapi_slot()` が持っていた
「`app.conf` の hsync の要求版 >= `kapi.json` の version」という検査を、
**「`sys_set_mtime` が入った v52 以上、かつ `kapi.json` の version 以下」**に
緩めた (H3 の挙動試験そのものは 1 件も変えていない)。
`test_hsync_h2.py` 側では逆に「52 ちょうどであること」を固定している。

## 確かめていないこと ([V4])

- **ゲスト (NP21/W) では 1 度も動かしていない。** 実 NHD / HostDrv 上の所要時間
  (票 §4-2 の 3)、大きい shlib の差し替え (同 4)、空き不足 (同 5)、強制終了からの
  復旧 (同 6) はすべて未実施。
- `make all` / `make check` / `make external` / 配備は**実行していない** (禁止範囲)。
  `make clean` → 全体ビルドは PM が着地時に回す ([ABI3])。
- 贋 FS は「名前 = ノード」の模型なので、**ext2 の媒体の状態** (links_count の
  過大計上、漏れ、公開の 3 値判定) は見ていない。そこは
  `tools/tests/b8_open_host.c` の段 C / 段 H の掃引と `e2fsck -fn` が受け持つ。
