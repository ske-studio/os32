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

---

# 追記: 回 5 (節 R) — 実機初回起動の差し戻し「FATAL: shell.bin load failed」

対象票: [`docs/tasks/gui/v13/TASK_K5B_kernel.md`](../../docs/tasks/gui/v13/TASK_K5B_kernel.md) (差し戻し 2026-09-11)
実機の観測 (PM、NP21/W、15MB 構成): `kselftest_pass=44 kselftest_fail=0` でカーネルは
起動しているのに、text VRAM 1 行目が `FATAL: shell.bin load failed` + shlib のロード
報告、EIP = `kernel_main+0xd4c` (`kernel/kernel.c:582` の停止ループ)。

| 回 | 対象 | RED | GREEN |
|---|---|---|---|
| 5 | `ext2_read_file()` が `max_size` を越えて書く | 実装前の `fs/ext2_file.c` に戻すと 4 検査中 3 検査が FAIL (`resolved[]` が読んだファイルの中身で潰れる) | `tools/tests/test_ext2_read_bound.py` 4 検査 ALL PASS |
| 5' | シェル帯 (ID 1) が per-app 経路に入らない | — (境界を固定する追加検査。K5b-K でも GREEN) | `tools/tests/test_multiapp_impl.py` ケース 18 (13 検査) ALL PASS |

## 原因 (推測ではなく実ソースと `kernel.map` で確定)

`fs/ext2_file.c:34` (K5b-K 前からある行) —

```c
ret = ext2_read_block(ctx, phys, &dst[total_read]);   /* 端数ブロックでも 1KB 書く */
```

`ext2_read_block()` は `dev_blk_read_lba` を 512B × 2 回呼ぶだけで、**`to_copy` に
関係なく必ず `EXT2_BLOCK_SIZE` = 1024 バイト書く**。`ext2_read_file()` は
`remaining` を `max_size` で頭打ちにしているが、書き込みの長さは頭打ちにしていない。
`max_size` が 1KB の倍数でない呼び出しは最大 1023 バイト溢れる。

K5b-K 前は exec が「ファイル全体をロード番地へ」読んでいたので、溢れ先は
そのプログラム自身の帯の中で無害だった。K5b-K が D1 の手順 2 のためにヘッダを
**先読み**するようにし (`exec/exec.c:996`)、宛先を

```c
static u8 hdrbuf[OS32X_HDR_V2_SIZE + 64];   /* = 108 バイト、カーネル .bss */
```

に変えた瞬間に顕在化した。`build/out/kernel.elf` の `.bss` は

```
00150900 0000006c b hdrbuf.2      <- ここへ 1024 バイト書かれる
00150980 00000100 b resolved.3    <- 解決済みパス。まるごと潰れる
00150a80 00000018 b g_exit_jmpbuf
00150ab4 00000004 b ring3_tramp_page
00150ac0 00002000 b ring3_tramp_raw   <- 先頭 0x240 バイトまで潰れる
```

で、`hdrbuf` の 108 バイトの直後に **`resolved[]` (解決済みパス) が居た**。
ヘッダ先読みが `resolved` をファイルの中身で塗り潰し、続く本体読み込み
`vfs_read(resolved, file_buf, read_max)` (`exec/exec.c:1298`) がゴミのパスを
引くので `sz <= 0` → **`EXEC_ERR_NOT_FOUND`**。`/sys/shell.bin` も FDD
フォールバック (`SYS_SHELL_BIN_FDD` は同じ綴り) も同じところで落ちるため、
`kernel/kernel.c:582` の `FATAL: shell.bin load failed` に落ちる。
`EXEC_ERR_NOMEM` の経路 (`[DBG] NOMEM: ...`) は**通っていない** — 画面に
その行が 1 つも出ていないことと符合する。

`14000` (実機の 2 行目) は別物ではなく、`kernel/shlib.c:191` の

```
[shlib] /sys/lib/libos32gui.shlib v1 loaded: 101 funcs, text 20 pg, data 4 pg @414000
```

が 80 桁で折り返した尻尾 (`@4` が桁 78-79、`14000` が次行)。`FATAL:` の 28 文字が
桁 0-27 を上書きしたので `shlib v1 loaded:` から見えている。**デバッグ出力ではなく
正規のロード報告なので残す**。

## 直したもの

1. `fs/ext2_file.c` — 端数ブロックは `ext2_g_blk` を中継して `to_copy` だけ写す
   (`ext2_read_stream` が前からやっている約束と同じ。`ext2_g_aux` は `ext2_bmap`
   が使うので不可 — gotcha §4-24)。ブロック境界ちょうどの読みは経路が変わらない。
2. `exec/exec.c` — 読み込み失敗時の `exec_cpl0_release()` を `!is_shell` で囲った。
   claim しているのは CPL=0 の**子**だけで、シェルは通っていない (現状は
   `g_cpl0_children <= 0` の門で無害だが、左右が揃っていなかった)。
3. `exec/appslot.c` / `.h` — `appslot_launch_is_app(is_shell, hdr_flags)` を足し、
   `exec_launch` の `want_ring3` をこれに置き換えた。シェル帯とアプリ帯の
   分かれ道が 1 か所になり、ホストで押さえられる (ケース 18)。

`is_shell` 経路の物理配置は **K5b-K でも既に元のまま**だった (PM の見当は外れ):
`MEM_SHELL_LOAD_ADDR` / `MEM_SHELL_MAX_SIZE` / `MEM_SHELL_STACK_TOP` /
`kapi->sbrk_heap_limit = guard_b`、`appslot_start_admit` も
`paging_addrspace_create_n` も `exec_cpl0_claim` も通らない。ここは変えていない。

## RED の作り方 (実際に走らせた)

```bash
git show HEAD:fs/ext2_file.c > fs/ext2_file.c     # K5b-K 時点へ戻す
python3 -B tools/tests/test_ext2_read_bound.py
```

```
FAIL overrun: max_size=108 wrote past +108 (offset +108)          EXIT bound_header=1
FAIL overrun: max_size=65536 wrote past +1500 (offset +1500)      EXIT bound_tail=1
  R3 block-aligned reads unchanged                                EXIT bound_aligned=0
FAIL: resolved[] clobbered: "()*+,-./0123456789:;<=>?@ABC..."     EXIT exec_bss_neighbour=1
SUMMARY 1/4 PASS
```

R4 の FAIL 文字列が実機の症状そのもの — `resolved[]` が読んだブロックの中身で
潰れている。直してから `SUMMARY 4/4 PASS`。

## 追加した検査

| 節 | 性質 |
|---|---|
| R1 | `max_size` が 1KB の倍数でない読み (exec のヘッダ先読みと同じ 108 バイト) が `max_size` を 1 バイトも越えない |
| R2 | ファイル末尾が端数ブロック (1500 / 1 / 1025 バイト) でも、返した長さより先を書かない |
| R3 | ブロック境界ちょうどの読みは従来どおり (経路を変えていない) |
| R4 | 108 バイトのヘッダバッファの直後に解決済みパスを置いた **実機と同じ並び**で、パスが生き残る |
| 18a-d | `appslot_launch_is_app()`: シェルは `flags` に関わらずアプリ帯を使わない / 子は従来どおり (`--cpl0` は identity) |
| 18e-k | 空き 1 枚 (GUI アプリは `EXEC_ERR_NOMEM`) でもシェルは起動し、空きページ・ID の池・資源の所有者をどれも動かさない |
| 18l-m | gshell ⇔ CUI shell の載せ替えを繰り返しても ID 1 / 段 1 のまま |

## 実行コマンド

```bash
python3 -B tools/tests/test_ext2_read_bound.py     # 回 5 (新規、4 検査)
python3 -B tools/tests/test_multiapp_impl.py       # ケース 18 を追加 (111 検査)
```

`build/sdk.mk` に `test_ext2_read_bound.py` (`check-vfs-mount-dev-host`) と、
K5b-K で登録されていなかった `test_multiapp_impl.py` / `test_owner_reclaim.py`
(`check-multiapp-model-host`) を足した。不要なら PM が外す。

## この追記で**測っていないこと** ([V4])

- **ゲストは未検証**。NP21/W では 1 度も動かしていない。実機の初回起動が直ったか
  どうかはテスターの再配備待ち。`kernel.map` の番地は動いている
  (`hdrbuf.2` = 0x150900 → 0x150aa0)、kselftest の読み出しは新しい地図で。
- 全体ゲート (`make all` / `make external` / `make check`) は回していない。
  通したのは `make kernel` (-Wall 警告ゼロ)、`tools/check_constraints.py` (EXIT=0)、
  および `test_ext2_read_bound` / `test_multiapp_impl` / `test_owner_reclaim` /
  `test_sbrk_tier` / `test_app_band_pde` / `test_vfs_mount_dev` の 6 本。
- ヘッダ先読みの宛先が 108 バイトのままである点は変えていない。溢れは FS 側で
  塞いだので安全だが、他の FS ドライバが同じ癖を持ち込まない保証はコードには無い
  (`fatfs` の `f_read` と `iso9660` / `hostdrv` は確認済みで、いずれも指定長を守る)。
