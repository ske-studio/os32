# pci_bind — RED → GREEN の記録

票: [`docs/tasks/v3/TASK_HAL_WIRING.md`](../../docs/tasks/v3/TASK_HAL_WIRING.md) §1-4
対象: `drivers/pci_bind_match.c` (一致規則と遷移) / `drivers/pci_bind.c` (実行と記録)
試験: `tools/tests/test_pci_bind.py` + `tools/tests/pci_bind_host.c`
実行: `make check-pci-bind-host` (`check-par` の列)

## なぜホストで固定するのか

**NP21/W には PCI が無い。** `pci_init()` は 0CF8h の読み戻しが一致しないので
「mech#1 absent」の 1 行で終わり、結線表は**エミュレータでは 1 行も走らない**。
実機の日に初めて通る経路にしたくない — 実機で初めて踏んだ罠は
2026-09-22 だけで 3 つあった (§4-51・§4-53)。

`probe` は呼び手が渡す関数ポインタなので、偽 driver を並べれば遷移は全部
踏める。I/O も PCI も要らない。

## 3 つの結果を分ける理由

probe が失敗したときに「装置に触っていない」のか「未知の状態で残した」のかで、
次にできることが違う。触っていなければ次の候補へ渡してよいが、**未知の状態の
装置を次の driver に渡すと、そちらが「自分が初期化した」と思い込んで DMA を
始める**。だから `PCI_PROBE_QUARANTINE` はその BDF の探索を打ち切る。

## RED

### RED 1 — 実装が無い

```
$ python3 -B tools/tests/test_pci_bind.py
tools/tests/pci_bind_host.c:15:10: fatal error: ../../drivers/pci_bind_match.c: No such file or directory
```

### RED 2 — QUARANTINE で打ち切らない (変異 1 / 2)

`if (rc == PCI_PROBE_QUARANTINE)` を潰すと、隔離された装置の次の候補が
呼ばれる。`quarantine_stops` が `called[1] == 0` で落ちる。

### RED 3 — 理由が候補をまたいで残る (変異 3)

`pci_bind_reason_reset()` を消すと、1 本目の driver が書いた
`IRQ_QUARANTINED` が、理由を書かなかった 3 本目の DECLINE の理由として
記録される。`lspci` が**別の装置の話**を出す。`reason_reset` が落ちる。

### RED 4 — 線の様子で弱いほうを名乗る (変異 7)

隔離とストームのマスクが両方立っているときに `STORM_MASKED` を返すと、
**再起動まで戻らない隔離が「再計算で外れるかもしれないマスク」に見える**。
`line_state` が落ちる。

## GREEN

```
$ python3 -B tools/tests/test_pci_bind.py --target --mutate
HOST GNU89 -Werror compile PASS (real drivers/pci_bind_match.c)
TARGET i386-elf GNU89 -Werror PASS
EXIT match_rules=0
EXIT next_order=0
EXIT decline_chain=0
EXIT quarantine_stops=0
EXIT reason_reset=0
EXIT line_state=0
EXIT multi_dev=0
SUMMARY 7/7 PASS
MUTATION 1 RED (1 件): QUARANTINE で打ち切らない (状態不明の装置を次の driver に渡す)
MUTATION 2 RED (1 件): QUARANTINE を記録だけして探索を続ける
MUTATION 3 RED (1 件): 候補ごとに理由を初期化しない (前の driver の理由が後に残る)
MUTATION 4 RED (3 件): class 欄を見ない (別の種類の装置に当たる)
MUTATION 5 RED (1 件): subclass 欄を見ない
MUTATION 6 RED (1 件): device 欄を見ない (同じベンダの別チップに当たる)
MUTATION 7 RED (1 件): 隔離とストームで**弱いほう**を名乗る (隔離を復旧済みに見せる)
MUTATION 8 RED (1 件): 16 以上の irq_line で hook を呼ぶ (線ではない値をシフトする)
MUTATION 9 RED (1 件): 未割り当て (0xFF) と 16 以上で hook を呼ぶ (線ではない値をシフトする)
MUTATION 10 RED (1 件): 理由を書かなかった DECLINE を「問題なし」と記録する
```

## 何を見ているか

| ケース | 見るもの |
|---|---|
| `match_rules` | 4 欄の AND。`0xFFFF` / `0xFF` は**任意** (素通し)。NULL は 0 |
| `next_order` | 表順に候補を返す。`from` から先だけを見る。表が空 / NULL |
| `decline_chain` | DECLINE → 次へ、OK で**止まる** (後続の probe を呼ばない)。全部 DECLINE なら**最後の**理由が残る。一致ゼロは `NONE` / `NO_DRIVER` で probe は 1 度も呼ばれない |
| `quarantine_stops` | QUARANTINE でその BDF を打ち切る。理由を書かなければ `RESET_FAILED`、書けばそれ |
| `reason_reset` | 理由は**候補ごとに**初期化される。書かなかった DECLINE は `DECLINED_UNSPECIFIED` であって `OK` ではない |
| `line_state` | **読む時点で合成する**: BOUND の後に線が隔離されても `result` は BOUND のまま `line_state` だけ変わり、線が戻れば OK に戻る (保存していない証拠)。両方立てば隔離が勝つ。**0xFF と 16 以上は hook を呼ばずに OK** |
| `multi_dev` | 同じ driver が 2 台 (別 BDF) に当たってよい。記録は 8 バイトちょうど (KAPI v60 の出力保護がこの大きさを通す) |

## この票で決めていないこと

- **`pci_bind_line_state_hook` の実体は実装 A のもの。** `irq_line_quarantined`
  / `irq_storm_masked` (u16 ビットマスク) を `PCI_LINE_BIT_*` に写すだけの
  3 行のアダプタを PM が合流時に差す。既定は NULL = `PCI_LINE_OK`。
- **`pci_bind_info` を CPL=3 へ出す KAPI (v60) は PM が末尾追記する。**
  いまはカーネルの起動行 (`[pci] … bound (ok) irq=05`) だけが外から読める。
- 82557 の driver 本体 (probe の中身) は別票 (L-B)。この票の表は**空**で、
  「候補が 0 本でも安全に回る」ことを起動のたびに踏むためだけに呼んでいる。
