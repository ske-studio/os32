# cirrus_win — Cirrus の窓の可否を物理地図で決める (ホスト TDD の記録)

教訓: [docs/POLICY_DEBUG.md](../../docs/POLICY_DEBUG.md) §4-34 (デバイス窓は物理地図で判定、RAM の上端で判定しない)
観測: [docs/archive/settings/TASK_S5.md](../../docs/archive/settings/TASK_S5.md) §6 R2 (Cirrus)
実装: `gfx/backend_cirrus.c` の `cirrus_win_usable()`
試験: `tools/tests/cirrus_win_host.c` + `tools/tests/test_cirrus_win.py` (`make check-cirrus-win-host`)

## 欠陥

`cirrus_win_usable()` は窓を張ってよいかを `sys_get_mem_kb() > base / 1024` (RAM の**上端**) で決めていた。
K6-RAM 以後、15MB 機 + 高位 RAM (NP21/W `ExMemory 16`) の上端は 17,408KB で、15-16MB の穴の中にある
バンク窓 F60000h (15,744KB) まで「RAM が届いている」と判定され、`cirrus_probe()` はボードの ID 判定の前に 0 を返す。
PEGC で直した §4-34 と同じ形。

## 様式

`test_pgalloc_range.py` と同じ形: `cirrus_win_host.c` が実物の `gfx/backend_cirrus.c` を `#include` し
(1 行も写さない)、周りだけを贋物にしてホスト ILP32 GNU89 (`gcc -m32 -nostdlib`、`int 0x80`) で走らせる。
同じソースを `i386-elf-gcc -Werror` でもコンパイルする ([C1])。

- `pgalloc_range_has_ram` = **贋の物理地図** (RAM の span 表)。実物と同じく範囲異常・未初期化は 1。
- `sys_get_mem_kb` = 構成の上端 (旧判定を RED にするため)。
- `wab_glue_xe10` = 贋グルー (実物の窓の番地 `include/wab_xe10.h` を入れ、`probe` の到達を数える)。

## 見ること

1. 15MB + 高位 1MB (上端 17,408KB): バンク窓 F60000h は張れる。物理地図に問い合わせている。PEGC の窓 F00000h も同じ答え。
2. 同じ構成でリニア窓 01000000h (2MB) は張れない (高位 RAM と本当に重なる)。窓の末尾 1 ページだけの RAM でも拒む。窓の直前・直後の RAM では拒まない。
3. 15-16MB を RAM にした構成: バンク窓は拒む。窓の先頭 1 ページだけでも拒む。
4. 8MB 機: 両方張れる。
5. pgalloc 未初期化: 張らない。
6. 大きさ 0・ページングの守備範囲 (PAGING_MAP_SIZE) の外・桁あふれは拒む。
7. probe の段: 15MB + 高位 RAM ではバンク窓を通ってリニア窓で落ち、グルーの ID 判定へ進まない。
   高位 RAM の無い 15MB 機は ID 判定まで進む。バンク窓だけ RAM なら進まない。

## RED → GREEN

- RED (修正前の `gfx/backend_cirrus.c`、2026-09-29):
  `ASSERT FAIL: cirrus_win_usable(BANK, BANK_N)` / `EXIT cirrus_win_host=1` (1 の最初の確認で落ちる)。
- GREEN (修正後): 7 段とも PASS、`EXIT cirrus_win_host=0`、`TARGET i386-elf GNU89 -Werror COMPILE PASS`。

## 否定側 (`--mutate`)

写しの木 (`mutpar`) で `gfx/backend_cirrus.c` を壊し、すべて RED になることを見る。

| 変異 | 壊し方 | 落ちた所 |
|---|---|---|
| `top_of_ram` | 旧判定 (`sys_get_mem_kb() > base / 1024`) に戻す | 1 (バンク窓) |
| `no_map` | 物理地図を見ない (常に張れる) | 1 (問い合わせの有無) |
| `end_short` | 問い合わせの末尾を切り上げない (末尾ページが落ちる) | 2 (末尾 1 ページの RAM) |
| `first_early` | 窓の前の 1 ページまで問い合わせる | 1 (F00000h、直前が RAM) |
| `no_paging_tail` | ページングの守備範囲の末尾を見ない | 6 |

## 範囲の外 (未検証)

- NP21/W 上での確認 (Cirrus 有効 ini、RAM 17,408KB) は未実施。この修正だけでは **リニア窓の判定で
  probe は引き続き 0** になる見込み (試験 7 の 1 つ目がそれを固定している)。Cirrus を使えるようにするには
  リニア窓の置き場所か構成の設計判断が要る。
