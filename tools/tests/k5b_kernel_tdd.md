# K5b-K TDD 記録 — アプリ 4 本の同時実行 (カーネル側、KAPI v44)

対象票: [`docs/tasks/gui/v13/TASK_K5B_kernel.md`](../../docs/tasks/gui/v13/TASK_K5B_kernel.md)
設計の正典: [`docs/tasks/gui/v13/TASK_K5_multiapp.md`](../../docs/tasks/gui/v13/TASK_K5_multiapp.md) §設計 D0〜D11 + 決裁 (2026-09-11)

新しい挙動ごとに **RED を先に見てから GREEN にした**。RED は「実装前の状態を
再現したビルド」で実際に走らせて記録している (後付けの偽装ではない)。RED 用の
ハーネスはスクラッチパッドに置いた使い捨てで、リポジトリには入れていない —
やり方は各回の「RED の作り方」に書いてあるので、同じ手順で再現できる。

| 回 | 対象 | RED | GREEN |
|---|---|---|---|
| 1 | P1/P2/P6 (per-app 物理の写像) | 実装前の `kernel/paging.{c,h}` に対して試験を組むと**コンパイルが通らない** | `tools/tests/test_app_band_pde.py` EXIT=0 |
| 2 | 所有者 ID による回収 (P3) | `shm_free_owned` から owner の判定を落とすと 4 検査が落ちる | `tools/tests/test_owner_reclaim.py` 27 検査 ALL PASS |
| 3 | AppSlot と印 (D0 の C1〜C6) | `appslot_park_check` の OP_WAIT ゲートと `appslot_resume_check` の印判定を落とすと 11 検査が落ちる | `tools/tests/test_multiapp_impl.py` 98 検査 ALL PASS |

---

## 回 1 — per-app 物理の写像 (P1 / P2 / P6)

**新しい挙動**: 同じ仮想 0x500000 にアプリごとに別の物理を載せる (設計 D1 の I5/I6)。

- `paging_addrspace_map_user_range_phys()` (P1) — 仮想 ≠ 物理で範囲を張る
- `paging_addrspace_clear_app_band()` (P2) — アプリ PT の identity コピーを全部落とす (**I6**)
- `paging_addrspace_free_user_range()` (P6) — 張った物理だけを PTE を辿って返す

**RED の作り方**: `tools/tests/app_band_pde_host.c` の新しい節 H を、実装前の
`kernel/paging.c` / `kernel/paging.h` のスナップショットに対してビルドする
(ホストハーネスの `paging_host_source.c` の差し替え元を旧ファイルにし、
`-I` の先頭に旧 `paging.h` を置いたディレクトリを入れる)。

**RED の出力** (2026-09-11):

```
app_band_pde_host.c:238:19: error: implicit declaration of function
    ‘paging_addrspace_clear_app_band’ [-Werror=implicit-function-declaration]
app_band_pde_host.c:255:19: error: implicit declaration of function
    ‘paging_addrspace_map_user_range_phys’; did you mean
    ‘paging_addrspace_map_user_range’? [-Werror=implicit-function-declaration]
app_band_pde_host.c:279:19: error: implicit declaration of function
    ‘paging_addrspace_free_user_range’ [-Werror=implicit-function-declaration]
cc1: all warnings being treated as errors
```

**GREEN**: `python3 -B tools/tests/test_app_band_pde.py` → EXIT=0

```
PASS: pde count rule (heap_size 0 = 1 pde, ram-capped, max clamped)
PASS: 1 pde layout identical to the current one
PASS: 2 pdes get private PTs, USER never reaches the master PDE/PT
PASS: bad count / out of pages roll back leaving master untouched
PASS: clear_app_band drops I6 identity, map_range_phys maps virt != phys
PASS: free_user_range returns only what it mapped, never shared PTs
PASS: app band selftest, keep/clone selftests still green
HOST ILP32 + TARGET GNU89 PASS
```

節 H が固定した不変条件:

- 生成直後のアプリ PT は master の identity コピーで、`clear_app_band` の後は
  **1024 エントリ全部が 0** (I6 = 「物理 0x5xxxxx が素通しで見える」を落とす)
- master の PT / PDE は 1 ビットも動かない。PDE は PT を指したまま present/RW、USER だけ落ちる
- `map_user_range_phys` は仮想 ≠ 物理で張り、USER は **そのアプリ PD の PDE にだけ**伝播する
- 物理 0 / 非整列 / 逆順は全範囲を未変更で拒否する
- `free_user_range` は張った枚数ちょうどを返し、PTE を 0 に戻し、
  二度呼んでも 0 枚 (二重解放しない)、共有帯 (VRAM) を渡しても 1 枚も解放しない

ブート時 `kselftest` から呼ばれる `paging_app_band_selftest()` にも同じ検査を
足した (ビット 4096 = P2、8192 = P1、16384 = P6)。

---

## 回 2 — 所有者 ID による回収が 1 本分で閉じる (D3 / P3)

**新しい挙動**: `shm_cleanup_all()` (所有者を見ない) を `shm_free_owned(id)` に置き換え、
`exec_exit` の回収 7 種を全部アプリ ID で回す。これが無いと、アプリ A の終了が
アプリ B の SHM ブロックを巻き上げる (4 本同時では実際に起きる)。

**RED の作り方**: `kernel/shm.c` の `shm_free_owned` から
`if (shm_block_owner[i] != owner) continue;` の 1 行を削り (= 旧 `shm_cleanup_all`
と同じ「所有者を見ない」挙動)、`tools/tests/owner_reclaim_host.c` をその
`shm.c` に対してビルドする。

**RED の出力** (2026-09-11):

```
  FAIL 3b ID 3 の SHM ブロックは使用中のまま
  FAIL 3c 属性を戻したのは ID 2 の 2 ブロックだけ
  FAIL 4c 無関係な ID の SHM はシェルの回収でも残る
  FAIL 5a owner 0 の回収は何も解放しない
FAILURES (4/27)
RED exit code: 1
```

**GREEN**: `python3 -B tools/tests/test_owner_reclaim.py` → 27 検査 ALL PASS

実物の `fs/fd_redirect.c` / `fs/pipe_buffer.c` / `kernel/shm.c` をそのまま
コンパイルしている (回収のロジックを試験側に写していない)。見ているもの:

- ID 2 と ID 3 が redirect / pipe / SHM を持った状態で **ID 2 だけ**を畳むと、
  ID 2 の分だけが消え、ID 3 の資源は 1 つも触られない
- SHM ブロックに確保時の ID タグが付き、返ったブロックのタグは消える
- GUI 予約ブロック (`SHM_RESERVED`、契約 T2) は誰の回収でも無傷
- 所有者 0 (タグなし) の回収は何も解放しない
- `shm_lock` 済みのブロックも所有者の回収で返る

FD (`vfs_close_owned`) と DB (`db_cleanup_owned`) は既に
`tools/tests/test_vfs_fd_sqlite.py` / `tools/tests/test_kapi_db_owned.py` が
同じ形で見ているので重複させていない。

---

## 回 3 — AppSlot と「OP_WAIT 由来」の印 (D0 の C1〜C6、受入 G7)

**新しい挙動**: `exec_ctx_stack`(段のスタック) を **ID で引く表** (`exec/appslot.c`) に
置き換え、park は `gui_call(OP_WAIT)` の中からだけ、resume は
「OP_WAIT で park された印のあるフレーム」に対してだけ成立させる。
WM の行儀を信じるのではなく**カーネルが弾く**。

**RED の作り方**: `exec/appslot.c` から
(a) `appslot_park_check` の OP_WAIT ゲート (`if (!g_cur_op_is_wait || !a->in_op_wait) {...}`) と
(b) `appslot_resume_check` の印判定 (`if (!a->parked_from_wait) {...}`) を削り、
`tools/tests/multiapp_impl_host.c` をその `appslot.c` に対してビルドする。

**RED の出力** (2026-09-11):

```
  FAIL 3a gui_call の外では park できない
  FAIL 3e OP_WAIT の中でだけ park できる
  FAIL 17a 印の無いフレームは OS32_ERR_STALE で拒否される
  FAIL 17b 拒否のたび ring3_resume_bad_frame_count が増える
  FAIL 17c 拒否は ring3_switch_count を増やさない
  FAIL 17d 拒否されたアプリは park のまま、WM は top-level のまま
  FAIL 17e 印のあるフレームは起こせる
  FAIL 17f 成功は switch_count だけを増やす
  FAIL 17g resume した時点で印は消える (二度は起こせない)
  FAIL 17h OP_WAIT 以外の op からの park は 2 回とも弾かれる
  FAIL 17i 弾いた回数が ring3_park_reject_count に載る
FAILURES
RED exit code: 1
```

**GREEN**: `python3 -B tools/tests/test_multiapp_impl.py` → 98 検査 ALL PASS
+ `TARGET i386-elf GNU89 -Werror COMPILE PASS`

内訳: K5a のホスト模型 (`tools/tests/multiapp_model_host.c`) の **84 検査**を
番号・検査名ごと写し、票 K5b が追加で求めた「印なし resume の負例」を
ケース 17 (14 検査) として足した = 98。ケース 1〜16 は模型と 1 対 1 なので、
落ちた検査名でそのまま「模型のどの規則が実装で崩れたか」が分かる。

模型との意図した差 (3 点。詳細は `multiapp_impl_host.c` の冒頭コメント):

1. 期待値は模型の `MA_ERR_*` ではなく **実物の `OS32_ERR_*` / `EXEC_ERR_*`**
2. ケース 6 (SHM スロット) と 12〜16 (同時 ready の選択規則) は設計 D6 / D11-5 が
   **gshell (WM) の領分**と決めた部分でカーネルには無い。ハーネス側に模型と
   同じ規則を置き、その下で park / resume だけを実物に通している
3. ケース 17 は票 K5b の追加要求 (C6 の負例) + CUI の入れ子の子は park できないこと

---

## 実行コマンド (`make check` への登録は PM)

```bash
python3 -B tools/tests/test_app_band_pde.py     # 回 1 (既存の的に節 H を追加)
python3 -B tools/tests/test_owner_reclaim.py    # 回 2 (新規)
python3 -B tools/tests/test_multiapp_impl.py    # 回 3 (新規)
```

`build/sdk.mk` の `check:` へは PM が足す (票の指示どおりこちらでは触っていない)。
足すときの並びの例:

```make
check-owner-reclaim-host:
	@python3 -B tools/tests/test_owner_reclaim.py

check-multiapp-impl-host:
	@python3 -B tools/tests/test_multiapp_impl.py
```

## この記録で**測っていないこと** ([V4])

- **ゲストは未検証**。NP21/W では 1 度も動かしていない (配備・エミュレータは
  この役の禁止範囲)。受入 G1〜G10 は PM とテスターの担当。
- 実際の GUI アプリの text+bss / `libos32gui.shlib` の .data ページ数は測っていない。
  D5 の見積表は仮置きのままで、8MB で GUI アプリが 1 本立つか (受入 G6) は実機の話。
- `pgalloc` の断片化。3 領域は連続で取れなければページ単位に倒すようにしたが、
  実際にどちらの経路を通るかは実機でしか分からない。
