# S2 — `libos32cfg` (設定レジストリのクライアント)、`cfg` コマンド、`cfg init`

状態: **設計 (PM 案、Codex 設計レビュー待ち)**。前提: S0-K (KAPI v50、`8bfe...`〜`a70df4f`)、S0-T (`/etc/settings.tsv` を通常配備、生成 DB は媒体だけ)、S0-D (通常配備は `/etc/settings.db*` を触らない)。決裁: [S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §3 (2 = 明示 `cfg init`、4 = S4 の最初の消費者は数キー)。
契約の正典: [S0_FOUNDATION.md](S0_FOUNDATION.md) §2 (非破壊 / transaction / 値の上限)、[DESIGN.md](DESIGN.md) §3 (スキーマ) / §4 (API) / §5 (起動時の振る舞い)。本票はそれを実装単位に切る。
規約: [C1] C89、外部プログラムは newlib 可、[V2] deploy.yaml、コーダーは worktree + ホスト TDD のみ。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S2-C** | `userland/lib/cfg/` = `libos32cfg` (静的、C89)、`userland/cmds/cfg.c`、ホスト TDD | C | `userland/lib/cfg/*`、`userland/cmds/cfg.c`、`build/libs.mk` (DEFINE_LIB の 1 行 + INC)、`tools/tests/` |
| **S2-W** | libos32gui の末尾追記 (cfg_* を GUI アプリへ公開、次の空きエントリから) | W | `userland/rust/libos32gui/src/*` (ffi / lib の表)、`sdk/rust/os32api` の宣言、`tools/check_gui_proto.py` の期待値 |
| 共有 (PM) | `userland/deploy.yaml` (`/usr/bin/cfg.bin`)、`build/app.conf` (`userland/cmds/cfg 50 0`)、`build/sdk.mk` (check 登録)、`tools/emu_agent/agent.py` | PM | — |

S2-C と S2-W は独立 (W は C の公開ヘッダに依存するので、C の `libos32cfg.h` を先に固定してから W を出す — ヘッダは本票 §1 で確定)。

## 1. `libos32cfg` の API (DESIGN §4 を確定、`userland/lib/cfg/libos32cfg.h`)

```c
typedef struct CfgDb CfgDb;                 /* 不透明。中身: KAPI handle、writable、schema_version、状態、txn */
#define CFG_OK       0
#define CFG_MISSING  1    /* DB が無い (fallback で動作) */
#define CFG_CORRUPT  2    /* 0 バイト / NOTADB / CORRUPT / hot journal (BUSY_RECOVERY) */
#define CFG_VERSION  3    /* schema_version が自分より新しい (読みは認識できる列だけ、書きは拒否) */
#define CFG_ERROR    4    /* I/O 等 (詳細は cfg_last_sqlite()) */

int   cfg_open(CfgDb **out, int writable);  /* /etc/settings.db。0 = 開けた (状態は cfg_status)。負 = OS32_ERR_* (引数不正・メモリ) */
void  cfg_close(CfgDb *db);                 /* 未 commit は rollback してから close */
int   cfg_status(const CfgDb *db);          /* CFG_* */
int   cfg_last_sqlite(const CfgDb *db);     /* 直前の SQLite 拡張コード (db_error_code の写し) */

int   cfg_get_int (CfgDb *db, const char *scope, const char *key, int def);
int   cfg_get_text(CfgDb *db, const char *scope, const char *key, char *out, int cap);   /* 戻り: 長さ (NUL 除く) / 負: OS32_ERR_NOTFOUND (無い→呼び手が既定値) / NOSPC (cap 不足: out は書かない) / INVAL */
int   cfg_get_blob(CfgDb *db, const char *scope, const char *key, void *out, int cap);  /* 同上 */
int   cfg_begin(CfgDb *db);                 /* BEGIN IMMEDIATE (writable のみ) */
int   cfg_set_int (CfgDb *db, const char *scope, const char *key, int v);
int   cfg_set_text(CfgDb *db, const char *scope, const char *key, const char *s);
int   cfg_set_blob(CfgDb *db, const char *scope, const char *key, const void *p, int n);
int   cfg_delete  (CfgDb *db, const char *scope, const char *key);
int   cfg_commit(CfgDb *db);  int cfg_rollback(CfgDb *db);
int   cfg_enum(CfgDb *db, const char *scope, const char *prefix,
               int (*fn)(const char *key, int type, void *ctx), void *ctx);   /* 戻り: 件数 / 負 */
int   cfg_init_from_tsv(const char *tsv_path, const char *db_path);           /* `cfg init` の実体: db_path が無いときだけ作る */
```

規則 (S0_FOUNDATION §2 を実装に写す):
1. **open**: `db_open_existing("/etc/settings.db", writable)`。失敗の `db_error_code(-1)` を CFG_* に写像: CANTOPEN → MISSING、NOTADB / CORRUPT → CORRUPT、BUSY_RECOVERY → CORRUPT (hot journal、自動回復しない)、他 → ERROR。開けたら `SELECT schema_version FROM meta` を prepare-only + step で読み、自分の `CFG_SCHEMA_VERSION` (1) より大きければ VERSION、`meta` が無い / 読めない → CORRUPT。**writable=1 でも open は BEGIN しない** (`cfg_begin` の明示呼び出しだけ)。MISSING / CORRUPT / VERSION でも `cfg_open` は 0 を返し (呼び手を分岐させない)、get は既定値、set / begin は拒否 (`OS32_ERR_INVAL`)。
2. **get**: `SELECT type, ival, tval, bval FROM settings WHERE scope=? AND key=?` を prepare-only + bind (`db_bind_text`) + step。型が違えば NOTFOUND 扱い (既定値)。text は SHM の row から private バッファへ**即コピー** (次の DB 操作で無効)。cap 不足は NOSPC で out を書かない。
3. **set / delete**: `cfg_begin` の後だけ (txn 外は INVAL)。`INSERT OR REPLACE INTO settings(scope,key,type,ival,tval,bval) VALUES(?,?,?,?,?,?)` を bind で (SQL 引用や hex 化はしない)。set 失敗で txn を failed にし、`cfg_commit` は拒否して rollback。値の上限: scope / key 63B、text 255B、blob 4096B、key は `[a-z0-9_]+(/[a-z0-9_]+)*`、scope は `system` / `gshell` / `user` / `app:[a-z0-9_]+` (S0-T の tsv と同じ規則を C で)。不正 UTF-8 / 埋め込み NUL は拒否。
4. **commit**: `COMMIT` (単一 statement) → `vfs_sync` 相当は S5 の実測で決める (DESIGN §6「同期」)。close は未 commit を rollback。
5. **enum**: `SELECT key, type FROM settings WHERE scope=? AND key LIKE ?||'%' ORDER BY key` を全件 private 配列 (最大 256 件、key 64B) に写してから callback (再入で cfg_* を呼んだら INVAL)。
6. **X4 / IRQ / 描画 callback から呼ばない** (ライブラリは検査できないので doc)。1 プロセス 1 接続。
7. **`cfg_init_from_tsv`**: `db_path` を `db_open_existing(..., 0)` で試し、**CANTOPEN (欠損) のときだけ**進む (0 バイト / 壊れは CORRUPT で拒否 = リカバリの領分)。tsv を S0-T と同じ規則の最小 reader (`userland/lib/cfg/cfg_tsv.c`、純関数、行長 8192+、4 列厳密、`#` / 空行、int32、hex blob、重複拒否) で読み、旧 `db_open` (CREATE あり、v42) で `db_path + ".new"` を作り、`db_exec` で DDL (DESIGN §3、`page_size=1024`、`journal_mode=DELETE`、`user_version=1`、meta に `schema_version=1` と `created=<tick or 'init'>`)、行は bind で挿入、close、`sys_rename(".new" → db_path)`。途中で失敗したら `.new` を消して非ゼロ。**既存があれば何もしない** (INVAL、`cfg` は "already exists")。
8. エラー番号は既存の `OS32_ERR_*` に写像 (新規番号を取らない)。

## 2. `cfg` コマンド (`userland/cmds/cfg.c`、`/usr/bin/cfg.bin`)

```
cfg get <scope> <key> [default]        int/text は値を、blob は hex を出す。無ければ default か "(not set)"
cfg set <scope> <key> int|text|blob <value>   1 トランザクション。blob は hex
cfg del <scope> <key>
cfg list [<scope> [<prefix>]]          scope/key/type/値
cfg status                             OK / MISSING / CORRUPT / VERSION (+ sqlite code) と schema_version
cfg init [--tsv /etc/settings.tsv]     無いときだけ /etc/settings.db を tsv から生成 (決裁 2)
cfg export <file> / cfg import <file>  DESIGN §6b の JSON 1 行 1 レコード (blob は base64) — **本票では export のみ**、import は S3 (JSON reader を持ち込む前に)
```
- GUI 配下 (端末) でも動く (KAPI だけ、TUI なし)。終了コードは 0 / 1。MISSING のときは `settings.db missing: run 'cfg init'` を出す (DESIGN §2 の文言は S3 のリカバリ用に残す)。

## 3. libos32gui への末尾追記 (S2-W)

- ジャンプ表の**次の空きエントリから**、`cfg_open` / `cfg_close` / `cfg_status` / `cfg_get_int` / `cfg_get_text` / `cfg_set_int` / `cfg_set_text` / `cfg_begin` / `cfg_commit` / `cfg_rollback` の 10 本 (blob / enum / delete は v1 の GUI 消費者 (S4) に不要なので後回し、必要になれば末尾追記)。既存エントリは不変。`tools/check_gui_proto.py` の期待値を更新。
- 実体は Rust から `libos32cfg` (C、静的) を呼ぶのではなく、**libos32cfg を libos32gui にリンク**して C の関数をそのまま表に載せる (libos32gfx と同じ形。W が現行の混在リンクの作法を確認)。

## 4. ホスト TDD (`tools/tests/`)

- `cfg_host.c` + `test_cfg.py`: 実 SQLite + 実 `os32_sqlite_vfs.c` + RAM backend (kapi_db_v50_host の作法) の上に `libos32cfg.c` を載せ、(1) MISSING → 0 / 既定値 / set 拒否、(2) CORRUPT (0 バイト、hot journal)、(3) VERSION (meta 2)、(4) get の型違い・cap 不足 (out 不変)、(5) begin → set → commit → close → reopen で読める、(6) set 失敗 → commit 拒否 → rollback、(7) close で rollback、(8) 上限 (63/255/4096、key / scope 規則、UTF-8)、(9) enum の件数・順序・再入拒否、(10) tsv reader の規則 (S0-T の test_mk_settings_db と**同じ fixture**を共有)、(11) `cfg_init_from_tsv` が欠損時だけ作り、既存 / 0 バイトでは何もしない、生成 DB のスキーマが `mk_settings_db.py` と一致 (`user_version`、meta、行)。RED → GREEN を `tools/tests/s2_tdd.md` に。
- `cfg.c` の引数解釈は純関数に切り出して同じハーネスで。

## 5. 受入 (ゲスト)

| ID | 試験 | 合格条件 |
|---|---|---|
| C1 | `cfg status` (DB 無し) | `MISSING`、終了 1、`/etc/settings.db` は作られない |
| C2 | `cfg init` → `cfg status` → `cfg list` | `OK schema_version 1`、tsv の 3 行 (`gshell desktop/color int 1` 等) が出る。もう一度 `cfg init` → `already exists` で不変 (hash) |
| C3 | `cfg set gshell desktop/color int 5` → `cfg get gshell desktop/color` → 5。`cfg set … text` 255B 境界、256B 拒否 | 値と拒否 |
| C4 | `cfg export /tmp/s.json` | 1 行 1 レコード、行数 = 件数 |
| C5 | 端末 (GUI) から `cfg list` / `cfg set` | CUI と同じ結果 (CPL=3 で KAPI v50 経由) |
| C6 | 通常配備 (`make deploy` + `hsync`、`deploy-nhd`) の後も `cfg get` の値が保持される (D0) | 値不変 |
| C7 | S5 の実測の一部: FEP 辞書常駐 (`ime on`) で `cfg get` を 50 回 → `db_mem_used` が戻る、`cfg` 1 回の tick | 記録 |

## 6. レビューで見てほしい点

1. §1 の写像 (SQLite 拡張コード → CFG_*) と FOUNDATION §2-1〜2-6 の一致 (特に hot journal を CORRUPT にして自動回復しない、open が BEGIN しない、set 失敗後の commit 拒否)。
2. get / set が v50 の prepare-only + bind だけで書け、旧 `db_exec` (先頭 statement のみ) を使う箇所 (DDL、COMMIT) が単一 statement になっているか。
3. `cfg_init_from_tsv` の「欠損のときだけ」判定と `.new` → rename の原子性 (ext2 の rename)、途中失敗で残骸を残さない。
4. SHM の row (`db_column_*`) の寿命と private コピー、enum の再入。
5. libos32gui 末尾追記の作法 (Rust 側の表に C 関数を載せる)。
6. tsv reader の C 実装と Python 生成ツールの規則一致 (同じ fixture)。
