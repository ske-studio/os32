# TASK_N3 — libos32host と wget / lpr / hclip / date -sync (OS32 側 C)

発行: PM (2026-09-14) / 状態: **設計 第 1 版 (Fable 設計レビュー待ち)**。正典: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) §2 (サービス) / §5 (利用者)、ワイヤ [TASK_N0.md](TASK_N0.md) 第 5 版 §1a (host_open/status/read/write/close の ABI)。依存: N1 (KAPI v51、受入済み)、N2 (Agent の PRINT/CLIP、受入済み)。**KAPI は変えない** (v51 のまま)。同梱: F6 の wget 再確認 (§6)、Agent の `/file/` トラバーサル N-fix + N2 残 non-blocker (§7)。

## 0. 範囲
- **`libos32host`** (`userland/lib/host/`、C 静的、`build/libs.mk` の `DEFINE_LIB`): host_* KAPI の AGAIN ループ (`sys_yield`) と多段のサービス手順を隠す薄い層。GUI 配下 (park) でも CUI (sys_halt 相当) でも `sys_yield` で待つ (host_test.c と同じ作法、K7/T8 で確立)。
- **コマンド** (`userland/cmds/`、`/usr/bin` = deploy.yaml、api 51): `wget`、`lpr`、`hclip`、`date`。いずれも libos32host の薄い CLI。
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

- **AGAIN ループの上限**: 各ループは `sys_yield` で待ち、`link_tick` が進める。STALE を受けたら即 `HOST_ELINK` (無限ループにしない)。`host_status` が業務結果を返すまで yield。
- **エラー**: `HOST_ELINK` (リンク断/未確立、STALE)、`HOST_ESERVICE` (業務 4xx/5xx で呼び手が失敗扱いすべきもの — get は http_status で返すので使わない、print/clip の 409/500/503)、`HOST_EINVAL` (引数)、`HOST_EIO` (KAPI の予期しない負値)、`HOST_EABORT` (sink/src が中断)。定数は `libos32host.h` (3 層)。
- **要求行の組み立て**: `host_open` は 1〜1400B。`GET <url>` の url が長いと 1400B 超 → `HOST_EINVAL`。宣言長は 10 進で付ける (`PRINT DATA 7 4096`)。

## 2. コマンド (`userland/cmds/`)
- **`wget <url> [file]`**: `host_get`。file 指定なら `sys_open(O_CREAT|O_TRUNC)` して sink が write、無ければ stdout。進捗はバイト数 (完了時に `<n> bytes`)。http_status が 200 以外なら `wget: <status>` + 非ゼロ終了 (404 等)。`HOST_ELINK` → `wget: host link down`。
- **`lpr <file>` / `lpr -`**: file を `host_print_stream` (src = ファイル / stdin を読む)。完了で `printed, <pages> pages`。名前は basename (無ければ `stdin`)。
- **`hclip get` / `hclip put <file>`**: get = `host_clip_get` (sink = stdout)。put = ファイル (≤4096) を `host_clip_put`。
- **`date`** (既存を拡張、無ければ新規): 引数無しは RTC 表示 (既存)。**`date -sync`** = `host_time` → `rtc_write` があれば設定、無ければ取得した時刻を表示だけ (`HOST_SERVICES_PLAN §5`)。`rtc_write` の有無は `sys_*` / kapi を確認して分岐 (無ければ「表示のみ、RTC 未対応」と明示)。

## 3. 登録
`build/libs.mk` (`DEFINE_LIB libos32host`)、`build/programs.mk` (4 コマンドが libos32host.a をリンク — cmds の明示規則、tar.elf と同じ形)、`build/app.conf` (wget/lpr/hclip/date api 51)、`userland/deploy.yaml` (cmds glob で自動、date は既存)、`docs/07_shell.md` にコマンド追記、`CLAUDE.md` のコマンド数、`docs/INDEX.md` 等の数値。

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
N3 の実装で Agent (`tools/host_agent.py`) を触るので、以下をまとめて直す (TASK_N2 §5(a) / §9):
- **`GET /file/` のパストラバーサル (優先度高)**: `--file-root` 未指定なら `/file/` は 403 (ホストの任意ファイルを読ませない)、指定時も `os.path.realpath` で root 配下に収まることを確認して `..` を拒否。ホスト Python 試験。
- N2 の残 non-blocker 6 件 (TASK_N2 §9): `_outfile` の明示 close、`--clip file:` 空パス拒否、`RealB64Spawn.wait_all()` timeout、B7 試験の「子未完了」assert 時間依存、root 実行時の OSError 試験 skip、rc≠0 の 503 本文に stderr。
