# TASK_KSTRING_BENCH — x86 で kstring を C 版に切り替えるかの実測

> 発行: PM (Claude Code `claude-opus-5`、2026-09-16) / 状態: **計画 (2026-09-16)**

基点: `feat/gui` = `2958ecc`。
引き継ぎ: [`../agents/HANDOVER_2026-09-16.md`](../agents/HANDOVER_2026-09-16.md) §6。
正典: [`ARM_GAUGE.md`](ARM_GAUGE.md) §6 (フラグ)、§9 (計測表)。

## 0. 目的

`lib/kstring_c.c` (移植準備 順序 4-b) を **x86 でも使うか**を、思い込みではなく実測で決める。
今は `build/kernel.mk:17` の `ifeq ($(ARCH),x86)` でアセンブリ版 `lib/kstring_asm.asm` を積んでいる。
C 版に一本化できれば、アセンブリの保守が 1 本減り、他アーキの移植で分岐が消える。

**この票は計測までを範囲とする。切り替えるかどうかは計測値を見てユーザーが決める。**

## 1. 前提 (確認済み)

- 両版は同じ契約を満たすことが `tools/tests/test_kstring_c.py` で保証されている
  (同じ実行ファイルに両方リンクし、戻り値とバッファ全体を突き合わせる)。
- 対象は 13 本: `kmemcpy` `memcpy` `kmemset` `memset` `kstrlen` `strlen` `kstrcmp` `strcmp`
  `kstrncmp` `strncmp` `kstrcpy` `kstrncpy` `memcmp` (`lib/kstring_asm.asm` の `global`)。
- `kstrcmp` の符号は `-fsigned-char` と C 版の `u8` 化で固定済み。日本語ファイル名の並び順は変わらない。
- 計時は `kapi->get_tick()` (100Hz)。前例は `userland/tests/blit_test.c:168`。

## 2. 進め方

1. **計測プログラム** `userland/tests/kstr_bench.c` (CPL=3 の普通の外部プログラム)。
   - 対象 13 本を、代表的な長さで回す: 4 / 16 / 64 / 256 / 1K / 16K / 256K バイト。
     アラインは「両方 4 境界」「片方 1 ずれ」の 2 通り。
   - 1 ケースにつき、合計 1MB 以上を処理するまで繰り返し、`get_tick()` の差で測る。
     1 ケースの所要が 30 ティック (0.3 秒) 未満なら繰り返し回数を倍にする (分解能の確保)。
   - 出力は 1 行 1 ケースの固定書式 `KSTR <関数> <長さ> <ずれ> <ティック> <回数>`。
     テスターが目で読む必要がないようにする。
   - `deploy.yaml` に登録 ([V2])。
   - **アプリからカーネルの kstring は呼べない**ので、計測対象のソース (`lib/kstring_c.c` と
     `lib/kstring_asm.asm`) をプログラム側にもリンクする。方法は
     `tools/tests/test_kstring_c.py` が両版を同居させている形をそのまま使う
     (関数名が衝突するので、片方を別名で参照する薄いラッパを `userland/tests/` に置く)。
     **出荷するソースそのものを測る** — 写しを作らない。
2. **測る条件**: NP21/W で 2 つ。
   - 既定の CPU 設定 (今の `np21x64w.ini` のまま)。
   - 高速側の設定 (P100 相当)。**ini の変更は [D2]**、手順はスキル `os32-emu-config`。
     変更したら**必ず元に戻す**。
   - エミュレータの絶対時間は実機の根拠にしない。**同じ条件での asm 版と C 版の比**だけを見る。
3. **判断の材料**: 関数ごとの比 (C 版 / asm 版) の表。
   - 目安: 全体で **1.5 倍以内**なら C 版に一本化する価値がある。2 倍を超える関数があれば、その関数だけ
     asm を残す形も選択肢 (分岐が残るので推奨はしない)。
   - どちらに決めても `ARM_GAUGE.md` §9 に数字を**追記**する。

## 3. 切り替える場合にやること (決まってから)

- `build/kernel.mk` の `ifeq ($(ARCH),x86)` 分岐を外し、全 ARCH で `lib/kstring_c.c` を積む。
  `lib/kstring_asm.asm` は**消さない** (比較の基準として残す。`tools/tests/test_kstring_c.py` が使う)。
- `make clean` → `make all`。カーネルの `.text` サイズの増減を記録する。
- ゲストで `kselftest_pass` / `kselftest_fail` を**新しい `kernel.map` の番地**で読む
  ([`POLICY_DEBUG.md`](../../POLICY_DEBUG.md) §2)。kstring のケースが落ちていないこと。
- 回帰 (`tools/emu_agent/tasks/regress.txt`) を通す。

## 4. 受入

| ID | 反例・操作 | 期待 |
|---|---|---|
| K1 | `kstr_bench` をホストでも組んで既知の入力で回す | 両版の結果が一致 (`test_kstring_c.py` と同じ契約) |
| K2 | 同じ条件で 3 回測る | ティック数のばらつきが 10% 以内。超えるなら繰り返し回数を増やす |
| K3 | 2 条件 (既定 / 高速) × 13 本 × 長さ × ずれ | 表が埋まり、抜けは「測っていない」と明記される ([V4]) |
| K4 | ini を変えた場合 | 測定後に元へ戻したことを確認 ([D2]) |

## 5. この票でしないこと

`kstrcat` / `kstrncat` / `memmove` / `strchr` / `strcspn` / `strspn` (元から C)、
`lib/kstring.c` の他の関数、SIMD や 386 固有の最適化の検討。
