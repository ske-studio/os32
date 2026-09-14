# TASK_N3 — libos32host と wget / lpr / hclip / date -sync (OS32 側 C)

発行: PM (2026-09-14) / 状態: **設計 第 2 版 (往復 1 の blocker 4 件を反映: open も AGAIN/FULL を yield 待ち、write INVAL は status で業務/引数を判別、date は内部コマンド影と rtc_write 不在のため外部 `hdate` に改名・表示のみ、Agent の GET http を非同期化。往復 2 待ち)**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §2 (サービス) / §5 (利用者)、ワイヤ [TASK_N0.md](TASK_N0.md) 第 5 版 §1a (host_open/status/read/write/close の ABI)。依存: N1 (KAPI v51、受入済み)、N2 (Agent の PRINT/CLIP、受入済み)。**KAPI は変えない** (v51 のまま)。同梱: F6 の wget 再確認 (§6)、Agent の `/file/` トラバーサル N-fix + N2 残 non-blocker (§7)。

## 0. 範囲
- **`libos32host`** (`userland/lib/host/`、C 静的、`build/libs.mk` の `DEFINE_LIB`): host_* KAPI の AGAIN ループ (`sys_yield`) と多段のサービス手順を隠す薄い層。GUI 配下 (park) でも CUI (sys_halt 相当) でも `sys_yield` で待つ (host_test.c と同じ作法、K7/T8 で確立)。
- **コマンド** (`userland/cmds/`、`/bin` = deploy.yaml の cmds glob、api 51): `wget`、`lpr`、`hclip`、`hdate`。いずれも libos32host の薄い CLI。
- **ホスト TDD** (`tools/tests/`): libos32host を贋 KAPI で (`cfg_host.c` の作法)。
- **ゲスト受入**: §6 (PM/テスター、kernel-lgy98-link + host_agent v2)。

## 1. libos32host の API (`userland/lib/host/libos32host.h`)
`kapi` は crt0/リンク時に渡る (既存 lib と同じ)。全関数は**リンクが立っていること**を内部で待つ (STALE が続けば `HOST_ELINK`)。戻り値は 0 / 負の `HOST_E*` (下記)。業務ステータス (HTTP の 404 等) は out 引数で返す (エラーにしない)。

| 関数 | 動作 |
|---|---|
| `int host_time(char out[20])` | `host_open("TIME")` → status ループ → read 19B → close。`out` は NUL 終端。0 / 負 |
| `int host_get(const char *url, host_sink_fn sink, void *ud, int *http_status, u32 *nbytes)` | `host_open("GET <url>")` → status ループ (`*http_status` に業務値) → **read ループ (AGAIN→yield、0=完了、>0 は `sink(ud, buf, n)` に渡す)** → close。`sink` が非 0 を返したら中断 (`HOST_EABORT`)。本文をバッファに溜めない (ストリーミング) ので >64KB でもメモリ一定 (F6 の実サービス経路) |
| `int host_print_text(const char *name, const char *buf, u32 len, u32 *pages)` | `PRINT OPEN <name> text` → read `job <id>` → `PRINT DATA <id> <n>` を 1400B 単位で write 分割 (各 status 200 を確認) → `PRINT CLOSE <id>` → read `pages <n>`。id は内部で持つ。業務 500/409 は `HOST_ESERVICE` + `*pages` 据置き |
| `int host_print_stream(const char *name, host_source_fn src, void *ud, u32 *pages)` | 同上だが本文を `src(ud, buf, cap)` から引く (lpr - / 大きいファイル用、メモリ一定) |
| `int host_clip_get(host_sink_fn sink, void *ud, u32 *nbytes)` | `CLIP GET` → read ループ → sink。503 = `HOST_ESERVICE` |
| `int host_clip_put(const char *buf, u32 len)` | `CLIP PUT <len>` (len ≤ 4096、超過は `HOST_EINVAL`) → write → status 200 |

- **AGAIN ループ (B1 込み)**: **`host_open` も `AGAIN` / `FULL` の間は `sys_yield` で待つ** (`link_host_close` の RELEASE が未 ACK の間 `rel_pending` で open が AGAIN を返すため — 多段 PRINT の close 直後 open がほぼ確実に踏む。カーネルの `link_wait_open` / host_test.c の `wait_up` と同形)。status / read / write も同じく `sys_yield`。`link_tick` が進める。
- **STALE / 生存性**: **最初の open だけ上限付き (既定 3 秒 = `sys_time` で計る) で STALE を待ち** (リンク確立待ち)、それ以降の STALE は即 `HOST_ELINK`。PROCESSING を返し続ける Agent への出口として**全体期限 (既定 30 秒)** を設け超過は `HOST_ETIMEOUT` (無限ループにしない)。`NOSYS` (NIC 無し) は `HOST_ENODEV` (ELINK と別文言)。
- **エラー**: `HOST_ELINK` (STALE/未確立)、`HOST_ENODEV` (NIC 無し = NOSYS)、`HOST_ETIMEOUT` (全体期限)、`HOST_ESERVICE` (print/clip の 409/500/503 — `*svc_status` に業務値、診断本文の先頭を小 sink で受けてよい)、`HOST_EINVAL`、`HOST_EIO` (予期しない負値)、`HOST_EABORT` (sink/src 中断)。print/clip 系の関数は `u32 *svc_status` (NULL 可) を揃える。定数は `libos32host.h` (3 層)。
- **要求行の組み立て**: `host_open` は 1〜1400B。`GET <url>` の url が長いと 1400B 超 → `HOST_EINVAL`。宣言長は 10 進で付ける (`PRINT DATA 7 4096`)。

## 2. コマンド (`userland/cmds/`)
- **`wget <url> [file]`**: `host_get`。**http_status を確定してからファイルを作る** (404 で空ファイルを残さない、nb4)。file 指定なら `sys_open(O_CREAT|O_TRUNC)` して sink が write (短書き込みはループ)、無ければ stdout。完了で `<n> bytes`。http_status 200 以外は `wget: <status>` + 終了 1。`HOST_ELINK` → `wget: host link down` + 終了 2。終了コード: 0 成功 / 1 業務 (非 200) / 2 リンク / 3 usage。
- **`lpr <file>` / `lpr -`**: file を `host_print_stream` (src = ファイル / stdin を読む)。完了で `printed, <pages> pages`。名前は basename (無ければ `stdin`)。
- **`hclip get` / `hclip put <file>`**: get = `host_clip_get` (sink = stdout)。put = ファイル (≤4096) を `host_clip_put`。
- **`hdate`** (新規、`hclip` と同流儀): `host_time` でホスト時刻を 1 行表示。**`date -sync` にしない** (B3: `date` は常駐シェル / sh.bin の内部コマンドで外部 `date.bin` を影にする、かつ KAPI に `rtc_write` が無く RTC は書けない)。**RTC 設定はしない (表示のみ)** — `rtc_write` は v52 以降、内部 `date` への `-sync` 統合は将来のシェル票。票から `date` 拡張と「deploy.yaml の date は既存」を撤回。

## 3. 登録
`build/libs.mk` (`DEFINE_LIB libos32host`)、`build/programs.mk` (4 コマンドが libos32host.a をリンク — cmds の明示規則、tar.elf と同じ形)、`build/app.conf` (wget/lpr/hclip/hdate api 51)、`userland/deploy.yaml` (cmds glob `/bin` で自動)、`docs/07_shell.md` にコマンド追記、`CLAUDE.md` のコマンド数、`docs/INDEX.md` 等の数値。

## 4. ホスト TDD
`tools/tests/host_lib_host.c` + `test_host_lib.py`: libos32host を `#include` し、host_* を**贋 KAPI** (状態機械: open→AGAIN×k→status→read チャンク→close をスクリプトで与える) に差し替え。踏む: get の AGAIN ループと sink 呼び出し・>64KB のストリーミング (メモリ一定)・http_status の受け渡し・sink 中断、print_text/stream の多段 (job 採番→DATA 分割→CLOSE→pages)・500/409、clip get/put・503・4096 超、STALE で `HOST_ELINK`、url 1400B 超で `HOST_EINVAL`。コマンドは `-DHOST_TEST` で引数処理・終了コードを (stat/tar と同じ作法で) 単体試験。

## 5. レビューで見てほしい点 (設計)
1. libos32host の AGAIN/yield ループが GUI (park) と CUI の両方で正しく待ち、STALE で無限ループしないか。
2. `host_get` / `host_print_stream` が本文をバッファに溜めず sink/src でストリーミングし、>64KB (F6) と大きいファイルでメモリ一定か。
3. print の多段 (OPEN→DATA×n→CLOSE) の途中失敗 (STALE / 500 / 409) の後始末 (開いた job をどうするか、ハンドルの close)。
4. 業務ステータス (get の 404、print/clip の 4xx/5xx) の呼び手への伝え方 (get は out、他は `HOST_ESERVICE`) が一貫か。
5. §7 の N-fix (Agent `/file/` トラバーサル) の直し方。

## 6. F6 の実サービス再確認 (受入で)
N1 の F6 (自己試験 L1/L2 が 64KB 超で 66114 B 停止) は「ゲスト固有のタイミング、コードは >64KB を扱える (ホスト TDD 35/35)」で保留した。N3 の受入で **実サービスの wget で 64KB 超を落として確認**: ゲストで `wget http://<64KB 超のファイル> /tmp/big` または Agent の `GET /pattern/200000` (host_get 経由) を流し、(a) 全量が落ちるか、(b) L1/L2 と同じ ~66114 B で切れるか。切れれば F6 は実害 → `link_tick` と consume の競合を計装して深掘り (別 N-fix)。切れなければ F6 は自己試験固有 (link_l1_bulk/link_l2_stream の LINK_IDLE ループ) の artifact として片付け、票に記録。

## 7. 同梱 N-fix (Agent、ホスト Python)
N3 の実装で Agent (`tools/host_agent.py`) を触るので、以下をまとめて直す (TASK_N2 §5(a)/(c) / §9):
- **`GET http(s)://` の非同期化 (B4、優先度高)**: 現状 `urlopen(timeout=10)` が主ループを塞ぎ、~6 秒超で `link_resync` → wget が「host link down」になり F6 の再確認も偽陰性になる。→ N2 の B-1 と同じ子プロセス/非同期経路に載せ、待ち中は STATUS に PROCESSING。Python 試験: 3 秒遅延する HTTP fixture で STATUS→PROCESSING、完了で 200。
- **`GET /file/` のパストラバーサル (優先度高)**: `--file-root` 未指定なら `/file/` は 403、指定時も `os.path.realpath(join)` が `os.path.commonpath([realpath(root), realpath(join)])` == `realpath(root)` に収まることを確認 (`..`・symlink 脱出を落とす、前方一致 `/root2` を通さない)。ホスト Python 試験。
- N2 の残 non-blocker 6 件 (TASK_N2 §9): `_outfile` の明示 close、`--clip file:` 空パス拒否、`RealB64Spawn.wait_all()` timeout、B7 試験の「子未完了」assert 時間依存、root 実行時の OSError 試験 skip、rc≠0 の 503 本文に stderr。
