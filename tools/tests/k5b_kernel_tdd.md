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

---

# 追記: 回 4 — sbrk 物理の二段構え (ユーザー決裁 2026-09-11)

対象票: [`docs/tasks/gui/v13/TASK_K5B_kernel.md`](../../docs/tasks/gui/v13/TASK_K5B_kernel.md) 作業 8
決裁: `docs/tasks/gui/v13/TASK_K5_multiapp.md` 末尾の決裁表「sbrk は二段構え (2026-09-11)」

| 回 | 対象 | RED | GREEN |
|---|---|---|---|
| 4 | sbrk 物理の二段構え | 段の判定を K5b-K の「常に最低分」へ戻すと 4 検査が落ちる / 枚数の勘定を外すと 2 検査が落ちる | `tools/tests/test_sbrk_tier.py` 27 検査 ALL PASS |

## 何を変えたか

K5b-K (`38266a7`) は per-app 物理にしたとき、`heap_size` 未指定の CPL=3
プログラムの sbrk に張る物理を最低分 `MEM_EXEC_SBRK_MIN` (256KB) へ固定した。
identity だった頃は帯の残りぜんぶ (≒1.4MB) が黙って sbrk に使えたので、
`malloc` を多用する CUI プログラム (`less` 等) が割を食う。決裁は二段構え:

- **段 1 (従来式)** — 3 領域 (本体+sbrk / exec_heap / スタック) + PD + アプリ PT を
  「sbrk 上端 = `guard_a`」で見積もった総ページ数が `pgalloc_free_pages()` に
  収まるなら、`sbrk_end = guard_a`。K5b-K 以前とまったく同じ範囲を張る。
- **段 2 (最低分)** — 収まらなければ `sbrk_end = code_end + MEM_EXEC_SBRK_MIN`。
- 段 2 でも収まらなければ `appslot_start_admit()` が `EXEC_ERR_NOMEM`。既存の経路で、
  切り詰めもスワップもしない。
- `heap_size` を明示したプログラムは**この分岐に入らない** (K5b-K のまま最低分)。
  要求した `exec_heap` を必ず渡すのが先で、sbrk を伸ばす余地はそこに無い。
- CUI (`exec_run` の入れ子) と GUI (`exec_start`) はどちらも `exec_launch()` の
  同じ場所を通るので、規則は 1 つ。

判定は `exec/exec.c` の 2 関数に切り出した:

```c
static u32 exec_ring3_pages(u32 load_base, u32 sbrk_end, u32 exec_heap_size,
                            u32 band_pdes);
static int exec_sbrk_pick_tier(u32 load_base, u32 code_end, u32 guard_a,
                               u32 exec_heap_size, u32 band_pdes,
                               u32 free_pages, u32 *sbrk_end);
```

`exec_ring3_pages()` は D5 の勘定式そのもの
(`(sbrk_end-load)/4K + exec_heap/4K + stack/4K + 1 + band_pdes`) で、
勘定の場所 (`appslot_start_admit` の直前) と段の判定が**同じ 1 本の式**を見る。

## どちらの段で走ったか (観測点)

KAPI にはしない。`fault_kill_count` と同じカーネルシンボルで、`emu_read_mem` で読む:

```c
volatile u32 exec_sbrk_tier_last;      /* 直近の CPL=3 起動が採った段 (1 or 2) */
volatile u32 exec_sbrk_tier_count[2];  /* [0] = 段 1 の累計、[1] = 段 2 の累計 */
```

数えるのは 3 領域を実際に張り終えた起動だけ (`app_map_region` が 3 本とも
成功した直後)。途中で失敗した起動は数に入らない。番地は毎ビルドの
`build/out/kernel.map` を見ること (固定値を控えない → `POLICY_DEBUG` §2)。

## 試験 (`tools/tests/test_sbrk_tier.py` + `sbrk_tier_host.c`)

`test_pgalloc_model.py` が `exec_child_claim` を切り出すのと同じ流儀で、
**`exec/exec.c` の当該 2 関数をテキストのまま切り出して**ホストへ差し込み、
`exec/appslot.c` と一緒に ILP32 freestanding でコンパイルして走らせる。
並行して書いた別式ではなく、出荷するコードそのものを見ている。
レイアウト (`code_end` / `guard_a` / `exec_heap_size`) は `exec_launch()` と
同じ式でハーネスが 1 つ組む (帯 1 枚、text+bss = 64KB)。

3 性質 = 27 検査:

| 節 | 性質 |
|---|---|
| case 1 (8 検査) | 空きが十分 → 段 1。`sbrk_end == guard_a` で、最低分より広い |
| case 2 (8 検査) | 空きが 1 ページ足りない → 段 2。`sbrk_end == code_end + 256KB` ちょうど。帯の残りが最低分より狭い縁では `guard_a` で頭打ちにし、その場合は従来式と同じものを張ったので段 1 と数える |
| case 3 (11 検査) | 段 2 でも足りない → `EXEC_ERR_NOMEM`。生存アプリ数・現在のアプリ・資源の所有者・既存 2 本の `state` と `pages` がどれも変わらない。段 1 の枚数では拒否される空きで段 2 の枚数なら立つ (= 二段構えが効く場面) |

### RED の作り方 (2 通り、どちらも実際に走らせた)

RED 用のハーネスは置いていない。`exec/exec.c` / `exec/appslot.c` を一時的に
K5b-K の状態へ戻して同じ試験を回す。

1. `exec_sbrk_pick_tier()` の本体から段 1 の判定を落とし、常に
   `*sbrk_end = code_end + MEM_EXEC_SBRK_MIN` を返す (= K5b-K の挙動)。
   → `1c` `1d` `1e` `1f` の 4 検査が FAIL、EXIT≠0。
2. `appslot_start_admit()` の `if (free_pages != 0 && pages > free_pages)` を外す。
   → `3d` `3j` の 2 検査が FAIL、EXIT=1。

どちらも戻して GREEN (27 検査 ALL PASS、`EXIT=0`) を確認した。

### 実行コマンド

```bash
python3 -B tools/tests/test_sbrk_tier.py    # 回 4 (新規)
```

`build/sdk.mk` は票の指示どおり触っていない。`check-memory-host` の並びに
足すのは PM の担当。

## この追記で**測っていないこと** ([V4])

- **ゲストは未検証**。NP21/W では 1 度も動かしていない。8MB 構成で GUI アプリを
  立てたときに実際どちらの段になるか、`less` の `malloc` が段 1 で楽になるかは
  実機の話 (受入 G6 と併せてテスターの担当)。
- 全体ゲート (`make all` / `make external` / `make check`) は回していない。
  ここで通したのは `make kernel` (EXIT=0) と上の 1 本だけ。
- 段 1 は「収まるなら張る」ので、空きをほぼ使い切る起動を許す。その直後の
  V86 バッキングや PT の動的確保が痩せる可能性は残る (決裁どおりの実装で、
  余白は取っていない)。実機で足りなくなるようなら余白の議論は改めて。
