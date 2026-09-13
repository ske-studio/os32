# S2 — `libos32cfg` (設定レジストリのクライアント)、`cfg` コマンド、`cfg init`

状態: **設計 第 3 版 (往復 2 の 6 件を反映、往復 3 = 最終待ち。§7 の直列化はユーザー決裁待ち)**。前提: S0-K (KAPI v50、`8bfe...`〜`a70df4f`)、S0-T (`/etc/settings.tsv` を通常配備、生成 DB は媒体だけ)、S0-D (通常配備は `/etc/settings.db*` を触らない)。決裁: [S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §3 (2 = 明示 `cfg init`、4 = S4 の最初の消費者は数キー)。
契約の正典: [S0_FOUNDATION.md](S0_FOUNDATION.md) §2 (非破壊 / transaction / 値の上限)、[DESIGN.md](DESIGN.md) §3 (スキーマ) / §4 (API) / §5 (起動時の振る舞い)。本票はそれを実装単位に切る。
規約: [C1] C89、外部プログラムは newlib 可、[V2] deploy.yaml、コーダーは worktree + ホスト TDD のみ。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S2-C** | `userland/lib/cfg/` = `libos32cfg` (静的、C89)、`userland/cmds/cfg.c`、ホスト TDD | C | `userland/lib/cfg/*`、`userland/cmds/cfg.c`、`tools/tests/` (libs.mk / programs.mk は PM) |
| **S2-W** | libos32gui の末尾追記 (cfg_* を GUI アプリへ公開、次の空きエントリから) | W | `userland/rust/libos32gui/src/*` (ffi / lib の表)、`sdk/rust/os32api` の宣言、`tools/check_gui_proto.py` の期待値 |
| 共有 (PM) | `userland/deploy.yaml` (`/usr/bin/cfg.bin`)、`build/app.conf` (`userland/cmds/cfg 50 0`)、`build/sdk.mk` (check 登録、SDK 配布ヘッダ一覧に `libos32cfg.h`)、**`build/programs.mk`** (`cfg.bin` のリンクに `-los32cfg`、shlib のリンクに `libos32cfg.a`)、**`build/libs.mk`** (`DEFINE_LIB,libos32cfg` + `INC_libos32cfg` + `ALL_LIB_ARCHIVES`)、`tools/emu_agent/agent.py` (B12) | PM (C / W が必要行を報告) | — |

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
int   cfg_close(CfgDb *db);                 /* 未 commit は rollback してから close。0 / 負 (rollback / close の最初の失敗コードを cfg_last_close_error() に保持、CfgDb は静的 1 本なので解放後も読める) */
int   cfg_last_close_error(void);           /* 直前の cfg_close の失敗コード (0 = 成功) */
int   cfg_schema_version(const CfgDb *db);  /* 読めた schema_version (VERSION 状態でも実版を返す、MISSING / CORRUPT は 0) */
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
               int (*fn)(const char *key, int type, void *ctx), void *ctx);   /* 戻り: 件数 / 負 (257 件以上は OS32_ERR_NOSPC、callback 内の cfg_* は INVAL) */
int   cfg_enum_scopes(CfgDb *db, int (*fn)(const char *scope, void *ctx), void *ctx); /* SELECT DISTINCT scope ORDER BY scope (list / export の全 scope 走査、B10) */
int   cfg_init(const char *tsv_path);       /* `cfg init` の実体。DB の場所は固定 /etc/settings.db (任意の db_path は取らない、B2) */
```

規則 (S0_FOUNDATION §2 を実装に写す):
1. **open (B1 / B3 / B4)**: 必ず **RO で開いて検査してから** RW に切り替える。(a) `db_open_existing("/etc/settings.db", 0)`。失敗は `db_error_code(-1)` を写像: CANTOPEN **かつ `sys_stat` が NOTFOUND** → MISSING、CANTOPEN で stat が成功 (長さ / 深さ / journal 名の容量超過など KAPI 側の拒否) → ERROR、NOTADB / CORRUPT → CORRUPT、BUSY_RECOVERY (hot journal) → CORRUPT (自動回復しない)、IOERR / 他 → ERROR。(b) schema 検査は 1 本の SQL (**WHERE 無し = 表全体**、往復 2 の 2): `SELECT COUNT(*), MIN(typeof(schema_version)), MAX(typeof(schema_version)), MIN(schema_version), MAX(schema_version), MIN(schema_version BETWEEN 1 AND 2147483647) FROM meta` を prepare-only + step し、**count == 1、両 typeof == 'integer'、min == max、範囲内 == 1** でなければ CORRUPT (0 行・複数行 (正常行との混在を含む)・不正型・範囲外・2^32+1 はすべて CORRUPT)。`meta` 表が無い (prepare が失敗) → CORRUPT。版が `CFG_SCHEMA_VERSION` (1) より大きければ **VERSION**、小さい (0 は BETWEEN で弾かれる) は無い。(c) writable=1 で **RO 検査が CFG_OK のときだけ** RO を close → `db_open_existing(..., 1)` → **同じ schema 検査をもう一度** (不一致なら close して ERROR)。VERSION / CORRUPT / MISSING では RW に切り替えず、VERSION は RO 接続を**保持**して読める状態のまま (往復 2 の 3)。RO 検査 → RW 切替の直列化 (FOUNDATION §2-4) は SQLite の lock が os32 VFS で no-op (F3b 後回し) なので**ライブラリでは実現できない**。→ **§7 のユーザー決裁**: v1 は「settings.db に触る接続は同時に 1 本 (gshell は起動時の読みだけ、書くのは `cfg` コマンドと設定変更時、同時に走らせない)」を**上位契約 (FOUNDATION §2-4) の v1 例外として決裁**する。決裁されなければ、カーネルに単一接続の門 (別 KAPI、v51) を足す票を先に切る。(d) open は **BEGIN しない**。MISSING / CORRUPT / VERSION でも `cfg_open` は 0 を返す (呼び手を分岐させない)。**VERSION は読める状態**: get / enum は認識できる列 (`settings` の 6 列) をそのまま読み、set / begin だけ拒否 (`OS32_ERR_INVAL`)。MISSING / CORRUPT は get が既定値 (NOTFOUND)、set / begin は拒否。
2. **get**: `SELECT type, ival, tval, bval FROM settings WHERE scope=? AND key=?` を prepare-only + bind (`db_bind_text`) + step (DONE で自動 finalize、次回は再 prepare)。型が違えば NOTFOUND 扱い。text / blob は SHM の row から private バッファへ**即コピー** (次の DB 操作で無効)。cap 不足は NOSPC で out を書かない。NULL と空 text / blob は**区別する** (FOUNDATION §2-5、往復 2 の 5): 型が text/blob で値が NULL (SHM の型情報が NULL) → `OS32_ERR_NOTFOUND` (未設定扱い)、空値 → 長さ 0 を返す。tsv の空 text は空値として格納する (NULL にしない)。get 失敗 (I/O) は `cfg_status` を ERROR にし既定値を返す。
3. **set / delete**: `cfg_begin` の後だけ (txn 外は INVAL)。set は `INSERT OR REPLACE INTO settings(scope,key,type,ival,tval,bval) VALUES(?,?,?,?,?,?)`、delete は `DELETE FROM settings WHERE scope=? AND key=?` (単一 statement)、いずれも bind。set 失敗で txn を failed にし、`cfg_commit` は拒否して rollback。値の上限: scope / key 63B、text 255B、blob 4096B、key `[a-z0-9_]+(/[a-z0-9_]+)*`、scope `system` / `gshell` / `user` / `app:[a-z0-9_]+` (S0-T と同じ規則を C で)。不正 UTF-8 を拒否。**C の文字列 API では終端 NUL より後の埋め込み NUL は検査できない** (長さ付きは blob だけ) と明記。
4. **begin / commit / rollback / close (B5)**: `cfg_begin` = `BEGIN IMMEDIATE` (writable で OK 状態のときだけ)。commit / rollback は単一 statement。**失敗コードは rollback を実行する前に保存** (成功した `ROLLBACK` は診断を 0 に戻すため)。`cfg_close` は未 commit なら rollback → `db_close`。rollback / close が失敗したら **最初の失敗コードを `cfg_last_close_error()` に残し、負を返す**; KAPI 側の slot は隔離 (F1) されているので再 close しない。CfgDb は 1 プロセス 1 本の静的領域 (malloc しない) なので、失敗後も `cfg_last_sqlite` / `cfg_schema_version` は読める。
5. **enum (B6 / B10)**: prefix は LIKE を使わず `WHERE scope=? AND substr(key, 1, ?) = ? ORDER BY key` (`db_bind_int(len)` + `db_bind_text(prefix)`、`_` / `%` を含む key でも前方一致)。全件を private 配列 (最大 256 件 × key 64B) に写してから callback。257 件目があれば NOSPC を返し callback は呼ばない。callback 内で cfg_* を呼んだら INVAL (再入フラグ)。`cfg_enum_scopes` は `SELECT DISTINCT scope FROM settings ORDER BY scope` (最大 32 件)。**list / export は callback の中では値を取らない** — key を集めてから callback の外で get する。
6. **X4 / IRQ / 描画 callback から呼ばない** (doc)。1 プロセス 1 接続。
7. **`cfg_init(tsv_path)` (B2 / B7 / B8 / B9)**: DB は固定 `/etc/settings.db` (任意パスは取らない → KAPI が拒否する深い / 長いパスの類は到達しない)。手順: (a) `sys_stat("/etc/settings.db")` が **NOTFOUND** であること (存在 / 0 バイト / IOERR は拒否: 存在は `already exists`、他は `needs recovery`)。(b) `/etc/settings.db-journal` が **存在しない**こと (残っていれば「journal が残っている、リカバリ (S3) の領分」で拒否、消さない)。(c) 前回の残骸 `/etc/settings.db.new` / `.new-journal` があれば **init 自身の一時名なので削除**してから始める (ただし同じプロセスで直前の `db_close` が失敗した後は削除しない。隔離 slot は owner 終了でも回収されない (`kapi_db.c` の F1 規則) ので「次回起動で解消」を根拠にはしない — 残骸の `.new*` を消してよいのは、そのプロセスが `.new` を一度も開いていないときだけ)。(d) tsv を最小 reader (`cfg_tsv.c`、純関数、**行長の上限を持たないストリーム処理**: 列ごとに上限 (scope 63 / key 63 / type 4 / value は text 255B・blob 8192 文字・int は先頭ゼロを畳みながら 10 桁まで) を数え、コメント行は改行まで読み捨てる — S0-T の生成ツールと同じ入力を同じ判定にする、往復 2 の 6) で全行検証してから、旧 `db_open("/etc/settings.db.new")` (CREATE、v42) で作り、`db_exec` を **1 statement ずつ** (`PRAGMA page_size=1024`、`PRAGMA journal_mode=DELETE`、`PRAGMA user_version=1`、`CREATE TABLE meta…`、`CREATE TABLE settings…`、`BEGIN`、meta の INSERT、行ごとの INSERT は bind、`COMMIT`)。(e) `db_close` が失敗したら `.new*` を**消さず** (接続が隔離中)、負を返す (`init failed: close`、次回起動時の (c) が片付ける)。(f) `sys_rename(".new" → "/etc/settings.db")`。**rename は原子的と仮定しない** (ext2 の rename は「新名追加 → 旧名削除」で、巻き戻し削除の戻り値を無視する `fs/ext2_dir.c:485`): 失敗したら両方を `sys_stat` する。**本体と `.new` の両方が存在する** (新名追加は成功し旧名削除と巻き戻しが失敗 = 同じ inode を 2 名が指し、ext2 はリンク数を増やしていない) ときは**どちらも消さず** `needs recovery` (`.new` を unlink すると inode が解放され本体が壊れる、往復 2 の 1)。本体だけ存在 → RO で開いて schema 検査 → 有効なら成功、無効なら消さずに `needs recovery`。本体が無ければ `.new*` を消して失敗。(g) 成功したら RO で開き直して schema 検査 (自己確認)。二重 init (端末と CUI) は「同時に走らない」前提 (協調型で `cfg init` は KAPI の合間にしか譲らず、S2 では直列を契約に明記)。
8. エラー番号は既存の `OS32_ERR_*` に写像 (新規番号を取らない)。

## 2. `cfg` コマンド (`userland/cmds/cfg.c`、`/usr/bin/cfg.bin`)

```
cfg get <scope> <key> [default]        int/text は値を、blob は hex を出す。無ければ default か "(not set)"
cfg set <scope> <key> int|text|blob <value>   1 トランザクション。blob は hex
cfg del <scope> <key>
cfg list [<scope> [<prefix>]]          scope 省略時は cfg_enum_scopes → scope ごとに enum (key を集めてから callback の外で get)
cfg status                             OK / MISSING / CORRUPT / VERSION (+ sqlite code) と実 schema_version (cfg_schema_version)
cfg init [--tsv /etc/settings.tsv]     無いときだけ /etc/settings.db を tsv から生成 (決裁 2)
cfg export <file> / cfg import <file>  DESIGN §6b の JSON 1 行 1 レコード (blob は base64)、全 scope (enum_scopes) — **本票では export のみ**、import は S3
```
- GUI 配下 (端末) でも動く (KAPI だけ、TUI なし)。終了コード: 成功 0、`get` で default を使ったときも 0 (値は出す)、MISSING / CORRUPT / VERSION で書けないとき 1、CFG_ERROR 1 (`status` は `ERROR sqlite=<code>` を出す)、close 失敗 1 (`close failed (<code>)`)。`list` / `export` の scope 上限 32 超は `too many scopes` で 1。`export` の 1 行目はヘッダ `{"schema_version":1,"exported":"<tick>"}`。MISSING のときは `settings.db missing: run 'cfg init'` を出す (DESIGN §2 の文言は S3 のリカバリ用に残す)。

## 3. libos32gui への末尾追記 (S2-W)

- ジャンプ表は `tools/mkshlib.py --check` が `.long os32gui_*` を抽出して本数を照合する (B11)。よって表に載せる名前は **`os32gui_cfg_open` … の wrapper** (Rust の `#[no_mangle] extern "C"` か C の thin wrapper) にし、実体の `cfg_*` (C、`libos32cfg.a` を shlib にリンク) を呼ぶ。追加は末尾 10 本: `cfg_open` / `cfg_close` / `cfg_status` / `cfg_schema_version` / `cfg_get_int` / `cfg_get_text` / `cfg_set_int` / `cfg_set_text` / `cfg_begin` / `cfg_commit` (+ `cfg_rollback` で 11 本)。既存エントリは不変。表の実体は `userland/gshell/src/shlib.rs` (現在 101 本) と SDK stub `sdk/rust/os32api/src/gui/stub.rs`、本数定数は両方を 112 に更新し、`mkshlib.py --check` が通ること (`check_gui_proto.py` は C/Rust の共有定数・構造体の検査で表の本数は見ない)。
- リンク: `build/programs.mk` の shlib 規則に `libos32cfg.a` を足す (PM の共有所有、W が必要行を報告)。C の静的ライブラリを shlib に混ぜる既存例は libos32gfx。

## 4. ホスト TDD (`tools/tests/`)

- `cfg_host.c` + `test_cfg.py`: 実 SQLite + 実 `os32_sqlite_vfs.c` + RAM backend (kapi_db_v50_host の作法) の上に `libos32cfg.c` を載せ、(1) MISSING → 0 / 既定値 / set 拒否、(2) CORRUPT (0 バイト、hot journal)、(3) VERSION (meta 2)、(4) get の型違い・cap 不足 (out 不変)、(5) begin → set → commit → close → reopen で読める、(6) set 失敗 → commit 拒否 → rollback、(7) close で rollback、(8) 上限 (63/255/4096、key / scope 規則、UTF-8)、(9) enum の件数・順序・再入拒否、(10) tsv reader の規則 (S0-T の test_mk_settings_db と**同じ fixture**を共有)、(11) `cfg_init` が NOTFOUND のときだけ作り、既存 / 0 バイト / journal 残存 / `.new` 残骸では規則どおり、生成 DB のスキーマが `mk_settings_db.py` と一致 (`user_version`、meta、行)、(12) RO 検査 → RW 再検査 (版 2 の DB を writable で開くと VERSION で set 拒否、読みは可)、(13) meta の 0 行 / 2 行 / TEXT / 2^32+1 が CORRUPT、(14) enum の `a_b` / `axb` と prefix `a_` (前方一致のみ)、257 件で NOSPC、(15) close 失敗のコード保持 (KAPI の close 失敗を模型で注入、rollback 前に保存)、(16) rename 失敗後の両名の状態 (模型で「新名あり・旧名削除失敗」を注入)、(17) 別接続による SHM 上書きの前に private コピーが済んでいる、(18) meta の正常行との混在 (`1` と `0`、`1` と `2^32+1`) が CORRUPT、(19) NULL の text が NOTFOUND・空 text が長さ 0、(20) rename 失敗で両名が同じ inode のとき `.new` を消さない (RAM backend の rename 模型に「新名追加成功・旧名削除失敗・巻き戻し失敗」と links_count を注入)、(21) tsv の 9000 桁の `0` と長いコメントが S0-T と同じ判定。tsv fixture は S0-T と共有 (先頭ゼロ、CR、コメント中の不正 UTF-8、末尾空欄、最大 blob 行 8328B を含む)。RED → GREEN を `tools/tests/s2_tdd.md` に。
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
7. 往復 1 の 12 件の反映: RO 検査 → RW、CANTOPEN + stat NOTFOUND、meta の 1 行 / integer / 範囲、VERSION は読める、close の戻り値と診断、substr 前方一致、journal 残存の拒否、`.new` の残骸と隔離、rename 失敗後の両名検査、enum_scopes と schema_version、os32gui_cfg_* wrapper と mkshlib --check、programs.mk / libs.mk の所有。

## 7. ユーザー判断が要る点 (往復 2 の 4)

- **接続の直列化の v1 例外**: FOUNDATION §2-4 は「schema 検査から RW 切替までを接続直列化で守る」とするが、SQLite の lock は os32 VFS で no-op (F3b 後回し) で、ライブラリからは直列化できない。提案: **v1 は「settings.db に触る接続は同時に 1 本」を運用契約にする** (gshell は起動時の読みだけ、書くのは `cfg` コマンドと設定アプリの OK 時、同時に走らせない。協調型で 1 本の KAPI 呼び出し中に別アプリは走らないので、open〜close を 1 つのイベント処理内で終える限り交互実行は起きない)。決裁されればこの票の §1-1 (c) のとおり、否なら v51 で「単一接続の門」KAPI を先に切る。
