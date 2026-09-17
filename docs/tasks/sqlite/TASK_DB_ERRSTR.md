# TASK_DB_ERRSTR — `db_last_error()` / `db_column_text()` がカーネル番地を返す

> 発行: PM (Claude Code `claude-opus-5`、2026-09-17) / 状態: **計画 (2026-09-17)**

基点: `feat/gui` = `45bdec8`。
発見: [`../test/TASK_TEST_RESULT.md`](../test/TASK_TEST_RESULT.md) §10 (段 2 のゲスト受入中)。
**カーネル層なので [`POLICY_DEV.md`](../../POLICY_DEV.md) §1 により新機能より先。**

## 0. 何が起きるか

CPL=3 のアプリが `db_last_error()` の戻り値を読むと **#PF で死ぬ**。

実測 (2026-09-17、ゲストで `db_test`):

    [ring3] #PF (CPL=3 / syscall) addr=0x002B8DE0 EIP=0x005005C7 -> kill app

`0x2B8DE0` はカーネル側の SQLite の帯 (`0x200000`〜`0x2FFFFF`) の中。
シェルの `$?` は **139** (例外で畳んだ) を返す。

## 1. 原因 (確認済み)

`kapi/kapi_db.c:727`:

```c
const char * __cdecl kapi_db_last_error(int handle)
{
    DbSlot *slot;
    if (handle < 0 || handle >= DB_MAX_CONNECTIONS) return "invalid handle";
    slot = &db_slots[handle];
    if (slot->cleanup_error != SQLITE_OK) return slot->cleanup_message;
    if (!slot->in_use || !slot->db) return "invalid handle";
    return sqlite3_errmsg(slot->db);
}
```

**4 つの return すべてがカーネル番地**を返す。

| 返るもの | 在り処 | CPL=3 から |
|---|---|---|
| `"invalid handle"` (2 か所) | カーネルの `.rodata` (`0x1xxxxx`) | **読めない** |
| `slot->cleanup_message` | カーネルの `.data`/`.bss` | **読めない** |
| `sqlite3_errmsg(slot->db)` | SQLite の帯 (`0x2xxxxx`) | **読めない** |

同じ形がもう 1 本ある。`kapi/kapi_db.c:694` の `kapi_db_column_text()` は
**正常系は共有メモリ** (`DB_SHM_PTR + info->data_offset`) を返すので正しいが、
**3 つのエラー経路だけ `return "";`** でカーネルの `.rodata` を返す。
`msg[0]` を見るだけで死ぬので、症状は同じで踏む機会が少ないだけ。

## 2. なぜ悪いか — いちばん必要なときに落ちる

- `CLAUDE.md:128` と [`POLICY_DEBUG.md`](../../POLICY_DEBUG.md) §4-13 が
  **「枯渇の診断は `db_last_error()` を必ず出す」**と指示している。
- 実機で任意の DB を調べる唯一の道具 `userland/tests/dbq.c:19` が、
  **エラーのときにこれを呼ぶ**。つまり DB が壊れている場面で道具が死ぬ。
- `userland/tests/db_test.c:341` も呼ぶ (これで露見した)。

## 3. 正しい形はすでにある

共有メモリの protocol には**最初からエラー用の欄がある** —
`sdk/include/os32/os32_kapi_shared.h:75` の `DB_ResultHeader.error_offset`
(「エラーメッセージのオフセット (0 = エラーなし)」)。`kapi_db.c:216` が実際に書き、
`userland/lib/db/libos32db.c:126` の `db_errmsg()` がそこを読む。
**共有メモリはアプリの PD に見えている**ので CPL=3 から読める。
`kapi_db_column_text()` の正常系が壊れていないのも同じ理由。

## 4. 直し方 (設計、実装は別)

**`__cdecl` の型は変えない。** 同じスロット・同じ prototype のままなので
[ABI2] にも触れず、KAPI の版数も上げない。**変えるのは「どこを指すか」だけ。**

1. DB の共有メモリに**診断文用の固定領域**を 1 つ置く (長さの上限つき)。
   場所と大きさは `os32_kapi_shared.h` の 1 か所で決める ([C4])。
   既存の `error_offset` と衝突させない — あれは 1 回の `db_exec` の結果に
   結びついた欄で、`db_last_error()` は**いつ呼ばれるか分からない**ため別に持つ。

   **配置**: 共有メモリは 16KB × 16 ブロックで、DB はブロック 0
   (`DB_SHM_BLOCK_SIZE` = 16KB) を使う。ブロック 12〜15 は GUI の予約。
   新しいブロックは取らず、**ブロック 0 の末尾**を診断用に切り出すのがよい。

       [0]                                    [16KB]
       | DB_ResultHeader | 列情報 | 結果データ | 診断 |
                                              ^ ここから末尾まで

   **ここが今回いちばん危ない所**。結果データの上限を見ている場所が
   `kapi/kapi_db.c` に**少なくとも 5 か所**ある (218 / 252 / 255 / 316 / 366 行、
   いずれも `DB_SHM_BLOCK_SIZE` から引き算している)。**1 か所でも直し漏れると
   結果データが診断領域を踏み潰す**。上限は新しい定数 1 本から導き、
   `DB_SHM_BLOCK_SIZE` を直接引き算している箇所を残さないこと。
   ホスト試験で「大きな結果を書いても診断文が壊れない」を見る (受入 E7)。
2. `kapi_db_last_error()` は返す前に**その領域へ写して**、領域のポインタを返す。
   写す量は上限で切り、必ず NUL で終える。
3. `kapi_db_column_text()` の 3 つの `return "";` も、同じ共有メモリ上の
   **空文字列**を指すようにする (カーネルの `.rodata` を返さない)。
4. `sqlite3_errmsg()` の寿命に依存しない。写した後は SQLite 側が何をしても安全。

## 5. 受入

| ID | 反例・操作 | 期待 |
|---|---|---|
| E1 | ゲストで `db_test` | **落ちない**。`db_test: PASS n/n`、`$?` = 0。今は `$?` = 139 |
| E2 | 無効な handle で `db_last_error(-1)` の戻り値を CPL=3 から読む | `"invalid handle"` 相当が**読める**。死なない |
| E3 | 結果セットが無い状態で `db_column_text()` を呼び `msg[0]` を見る | 空文字列として読める。死なない |
| E4 | ゲストで `dbq` に壊れた DB を渡す | エラー文が画面に出る。落ちない |
| E5 | 上限を超える長いエラー文 | 切り詰められ、必ず NUL で終わる。溢れない |
| E6 | ホスト試験 (`tools/tests/` に新設) | 返るポインタが**共有メモリの範囲内**であることを全経路で確かめる。範囲外を返す変異で RED |
| E7 | 結果データを上限いっぱいまで書いた直後に `db_last_error()` | 診断文が壊れていない。上限の引き算を 1 か所戻す変異で RED |

E6 が肝。「読めた」ではなく**「返り先が共有メモリの中か」**を見る —
ホストでは番地の区別が付かないので、範囲の検査として書く。

## 6. この票でしないこと

- KAPI の型・スロット・版数の変更。
- `db_errmsg()` (userland 側) の作り直し。今のままで正しい。
- SQLite 本体、プールの大きさ、接続管理。
- `CLAUDE.md` / `POLICY_DEBUG.md` の指示の書き換え — **直った後**に、
  どちらを使うべきかを 1 行で整理する (直す前に書き換えると指示だけ先に動く)。
