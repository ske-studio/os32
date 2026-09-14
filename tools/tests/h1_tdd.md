# H1 ホスト TDD の記録 (hsync の同サイズ更新の検出)

票 [docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md](../../docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md)
§8 の H1 行 / §9 の A01〜A13。実行:

```
python3 -B tools/tests/test_hsync_h1.py [--target]
make check-hsync-h1-host        # 同じもの (--target 付き)
```

- `tools/tests/hsync_h1_host.c` … 実物の `userland/system/hsync.c` を 1 行も写さず
  `#include` する (`main` を `hsync_main` へ改名するだけ)。差し替えるのは KernelAPI
  だけで、オンメモリの贋ファイルシステムへ向ける。ノードごとに
  **short read の分割幅 / 読み取り途中の I/O エラー / 早期 EOF / 1 回の write 上限 /
  0 進捗 write / 書き込み時のビット反転 (破損) / stat のエラー・種別ゼロ・サイズ偽装**
  を注入できる。`vfs_sync` の失敗は大域スイッチ。`kprintf` は全文をログへ溜め、
  検査は**固定文字列の理由コード**で行う (文言の雰囲気で判定しない)。
- `fs/hostdrv_stat_rules.inc` (HostDrv の stat 失敗の是正) は純関数なので同じ翻訳単位で
  直接叩く。`fs/hostdrvfs.c` の `hdrv_stat()` はこの `.inc` を `#include` して呼ぶだけで、
  判定の写しをどこにも作っていない。
- CRC は `lib/crc32.c` (既存の一括版 `crc32_calc`) を同じ実行ファイルへリンクし、
  ストリーム核 (`lib/crc32_core.inc`) との一致を既知ベクトルと実データで見る。
- ビルドは 2 本。`-DHSYNC_CRC_STUB` を付けると CRC 核を「常に同じ値を返す贋物」へ
  差し替える (A08 の否定側)。

エミュレータ・実配備・`make` には一切触れない。

## 受入項目との対応

| ID | 内容 | 対応する検査 |
|---|---|---|
| A01 | 同サイズで 1 バイト変更 | `case_a01_a02` — `UPDATE /bin/a.bin reason=content_changed size=100` |
| A02 | 同サイズ・同 mtime で内容だけ変更 | `case_a01_a02` — 贋 stat は両側 `st_mtime=1000` を返す。それでも `content_changed` |
| A05 | 空 / 新規 / 大小変化 / バッファ境界 / 複数チャンク | `case_a05` — 0,1,2,32767,32768,32769,65535,65536,65537,131072,131085 の 11 サイズ x (同一 / 末尾 1B 差 / 先頭 1B 差 / 新規 / サイズ差) |
| A06 | 両側で異なる short read 分割・read エラー・早期 EOF・0 進捗 write・短い write | `case_a06` |
| A07 | CRC 既知ベクトル (1B 刻み / 全量 / 不規則チャンク) | `case_a07` — 空列 `00000000`、`123456789` → `CBF43926`、70000B をビット版の基準値と照合。チャンクごとの完成 CRC を XOR したものが**一致しない**ことも見る |
| A08 | CRC を差し替えて同値を返してもバイト比較が不一致を検出 | `case_a08` (通常ビルドと `-DHSYNC_CRC_STUB` の両方で走らせる) |
| A09 | copy 後の破損・`vfs_sync` 失敗 | `case_a09` — どちらも `reason=verify_failed`・`copied=0`・非ゼロ終了 |
| A10 | 強制モードでも settings.db の大小文字名・祖先保護・実体 (hardlink) 保護 | `case_a10` |
| A11 | stat 失敗・64bit サイズ | `case_a11_rules` (純関数) と `case_a11_hsync` (上位が空ファイル扱いしない) |
| A12 | 全体同期 / 明示 sys / `usr/sys` / 正規化 dir / root 外 dir / 自己コピー / 引数 | `case_a12` |
| A13 | dry-run で書込みが起きない | `case_a13` — `sys_write` / `sys_mkdir` / `vfs_sync` / `O_CREAT` の呼び出し回数がすべて 0 |

A03 / A04 (mtime) は票 H3、A14 / A15 (一時ファイルと置換) は票 H2、A17〜A21
(競合・manifest・列挙上限・性能) は H4 以降なのでここでは扱わない。

## RED → GREEN

先に GREEN を貼る。

```
$ python3 -B tools/tests/test_hsync_h1.py --target
CONST COUPLING PASS (VFS_MAX_PATH_DEPTH=32 == HS_MAX_PATH_DEPTH, HSP_MAX_DEPTH=32)
HOST GNU89 -Werror COMPILE PASS (real hsync.c)
...
188 checks, 0 failures
EXIT hsync_h1_host=0
HOST GNU89 -Werror COMPILE PASS [CRC stub] (real hsync.c)
...
3 checks, 0 failures
EXIT hsync_h1_host_crcstub=0
TARGET i386-elf -Werror COMPILE PASS (hsync.c)
TARGET i386-elf -Werror COMPILE PASS (lib/crc32.c)
TARGET i386-elf -Werror COMPILE PASS (fs/hostdrvfs.c)
```

RED は**実装を 1 か所ずつ「直す前」へ戻した変異**で取った ([V4]: 時系列で先に
赤い試験を書いた項目と、実装後に変異で確かめた項目が混ざっている。下表は後者を
含む「この試験が何を捕まえるか」の証拠であって、開発順の記録ではない)。

| 変異 | 戻した内容 | 落ちた検査 |
|---|---|---|
| M1 | 同一判定を「サイズが同じならスキップ」(2026-09-14 以前の実装) へ | **149 中 41 失敗** — A01 / A02 / A05 の全「1 バイト差」/ A06 の分割・エラー注入 / A08 / A09 / A13。CRC 贋物ビルドも 3 中 2 失敗 |
| M2 | `copy_verify` の読戻し検証を省く (書けたら成功) | 6 失敗 — A09 の全項目 (破損・`vfs_sync` 失敗・正常時の sync 回数) |
| M3 | `/sys` の除外を「全体同期か」を見ずに `depth==0` で効かせる | 1 失敗 — `hsync usr は usr/sys を除外しない` (A12) |
| M4 | dry-run でも書く | 7 失敗 — A13 の PLAN 行・`sys_write`/`vfs_sync` 回数・宛先内容・名前空間 |
| M5 | `hdrv_stat_fill` が Basic/Standard 取得失敗でも成功を返し、64bit を切り詰める (直す前の `hdrv_stat`) | 3 失敗 — A11 の 3 項目 |

M1 の RED (抜粋):

```
### M1_size_only  rc=1  totals=[('149', '41'), ('3', '2')]
    FAIL UPDATE ... reason=content_changed size=100 を出す
    FAIL 同 mtime でも content_changed でコピー
    FAIL size=65536 末尾 1 バイト差 -> copied
    FAIL 分割が違っても末尾の差を検出
    FAIL 予定サイズに達しない EOF -> エラー (同一扱いにしない)
    FAIL reason=verify_failed を出す
    ...
```

## 試験が言っていないこと

- ゲスト (NP21/W) 上での動作は一切見ていない。贋ファイルシステムは ext2 でも
  HostDrv でもない。実配備の受入 (A20: 同サイズ shlib の差し替え → 再起動 → GUI 起動) は
  PM とテスターの担当。
- 読戻しはキャッシュを経由する。媒体からの物理再読の保証ではない (票 H2/H4)。
- **H1 は宛先を直接上書きする。**コピー・検証の失敗時に旧宛先が残る保証は無い。
  試験もそれを前提に「失敗を成功件数に入れない」ことだけを見ており、
  旧内容の保存は見ていない (A14 は票 H2)。
- 性能 (A21) は測っていない。CRC 表サイズの選定根拠はホスト x86-64 上の相対値
  (`lib/crc32_core.inc` の頭に記録)。PC-98 実機・エミュレータでの測定ではない。

## 追記 2026-09-15 — Codex 実装レビューの blocker 3 件 (B1 / B2 / B3)

着地 (`e43fcb8`) 後のレビューで 3 件。PM が 3 件とも到達可能性を独立に確認済み。
修正と試験を同じ作法 (実物のソース + 変異で落ちること) で足した。

| ID | 直した内容 | 主な対象 |
|---|---|---|
| B1 | `\` を含むパスを断つ。OS32 の区切りは `/` だけなので `..\other` は**普通の 1 要素**として `..` 脱出検査を素通りするが、HostDrv の `session_set_path` が `/`→`\` に直したうえで他の文字をそのまま通すため、ホスト側では `C:\os32\..\other` になる。NP21/W の `hostdrvNT_getHostPath` は `PathCanonicalizeW` のあと**境界を見ない前方一致**でルート検査をするので `C:\os32-other` が通る。`argv` の dir と**列挙で得た名前の両方**を拒否 (`reason=bad_name`) | `hsp_has_backslash` / `hsp_normalize` / `ls_cb` / `main` |
| B2 | `/host` を前置した**あと**の要素数で上限を見る。`hsp_normalize` の `HSP_MAX_DEPTH` は入力側にしか掛からないので、32 要素の dir は通り、同期元は 33 要素になる。`fs/vfs.c` は上限超過の要素を**黙って捨てる**ので「指定した親ディレクトリ」を列挙してしまい、宛先側も化けて、何も同期していないのに `errors=0` / 終了コード 0 だった。越えたら `reason=path_too_deep` で `errors` に数える。あわせて **`PROTECTED` を「保護に当たったとき」だけの語**にし、正規化失敗は `reason=path_rejected` のエラーにした (`hsp_path_protected` の真偽 1 本を 3 値の `hsp_path_classify` に置き換え) | `hsp_depth` / `hsp_path_classify` / `HS_MAX_PATH_DEPTH` / `sync_directory` / `main` |
| B3 | `sys_mkdir` が `OS32_ERR_EXIST` を返したら宛先を `stat` して**ディレクトリであることを確かめる**。`fs/ext2_dir.c` の `ext2_find_entry` は**種別を問わず** EXIST を返すので、コピー元がディレクトリ・宛先が通常ファイルのまま `errors=0` / 終了コード 0 で終わっていた。`-n` は `mkdir` を呼ばないので、**dry-run でも型検査だけは行う** (書き込みはしない) | `dst_dir_type_ok` / `sync_directory` |

追加した検査は **`case_b1` 10 件 / `case_b2` 18 件 / `case_b3` 11 件 = 39 件**
(`hsync_h1_host.c`、149 → **188 checks / 0 failures**、CRC 贋物ビルドは 3 / 0 のまま)。
`hsync_protect_host.c` にも `.inc` の純関数ぶん (`hsp_has_backslash` /
`hsp_depth` / `hsp_path_classify` の 3 値) を **20 件**足した
(114 → **134 checks / 0 failures**)。既存の検査はすべてそのまま通っている。

`test_hsync_h1.py` は走らせる前に **[C4] の結合検査**を 1 本足した:
`fs/vfs.h` の `VFS_MAX_PATH_DEPTH` と `userland/system/hsync.c` の
`HS_MAX_PATH_DEPTH` を読んで一致を確かめ、`HSP_MAX_DEPTH` がそれを越えていない
ことも見る (外部プログラムから `fs/vfs.h` を引けないので写しを置くしかない。
写しが本体より大きいと化けたパスを通してしまう)。

### RED (変異)

`tools/tests/test_hsync_h1.py` を回した結果。括弧内は
`tools/tests/test_hsync_protect.py` での失敗数。

| 変異 | 戻した内容 | 落ちた検査 |
|---|---|---|
| B1 | `hsp_has_backslash` を常に 0 にし、`hsp_normalize` の `\` 検査を外す | **188 中 6 失敗** — `reason=bad_name` を出す / 要素の途中の `\` / 列挙名の `\` (非ゼロ終了・reason・名前を作らない) / `'\' 混じりも -1` (protect 側は 4 失敗) |
| B2a | `/host` を足したあとの深さ検査を 2 か所とも外す | 4 失敗 — off-by-one の境界 / `reason=path_too_deep` / 越えた先へ書かない / 再帰の途中の超過 |
| B2b | `hsp_path_classify` が正規化失敗で `-1` でなく `1` (= 保護) を返す | 2 失敗 — `33 要素は -1` / `'\' 混じりも -1` (protect 側は 2 失敗: `正規化できない長さは -1`) |
| B3 | `mkdir` の `EXIST` を無条件に受理する (dry-run の型検査も外す) | 7 失敗 — 非ゼロ終了 / `reason=type_conflict` / errors に数える / 中身があっても衝突 / 衝突した先へ書き込まない / dry-run の 2 件 |

B2b は経路としては深さ検査と `\` 検査が先に効くため、end-to-end だけでは
変異が落ちない。3 値そのものを直に呼ぶ検査 (`hsp_path_classify(...) == -1`) を
足して固定してある — **「判定できない」を「保護」に畳まない**ことが要点で、
`PROTECTED` が 2 つの意味を持たないこと自体を守っている。

### 直せなかった非 blocker

ホスト試験は **LP64 のまま**。`gcc -m32` に必要な 32bit ヘッダ
(`gcc-multilib` / `bits/libc-header-start.h`) がこの環境に無く、パッケージの
導入はこの票の範囲外なので入れていない。したがって `u32` (= `unsigned long`) は
ホスト上で 64bit であり、**32bit の加算回り込みはこの試験では未検証**。
CRC の値照合・分岐・件数の検査には影響しないが、桁溢れの根拠には使えない。
型を試験に合わせて変えることはしていない (実機は ILP32 のまま)。
実機幅での確認は `--target` の `i386-elf-gcc -Werror` コンパイルと、
ゲスト上の受入 (A20) に委ねる。
