# kbd_status — RED → GREEN の記録

票: 実機の打鍵不達 (2026-09-23)、経緯は [`docs/POLICY_DEBUG.md`](../../docs/POLICY_DEBUG.md) §4-57
対象: `drivers/kbd_status.c` (判定) / `drivers/kbd.c` (IRQ1 ハンドラ・`kbd_init`・`kbd_diag`)
試験: `tools/tests/test_kbd_status.py` + `tools/tests/kbd_status_host.c`
実行: `make check-kbd-status-host` (`check-par` の列)

## 何を直しているか

実機 PC-9821Ra266 で本体キーボードの打鍵が一切届かなかった (シリアルは動く、
NP21/W では効く)。本命の原因は `kbd_init` が 0043h に書いていたコマンド語
0x14 (DTR = 0 = RTY# LOW = キーボードへの再送要求) で、これは 0x16 に直した
(ホストでは試せない — kselftest が語の値を見張る)。

同じ変更で IRQ1 ハンドラが **0041h を読む前に 0043h を見る**ようにした。
直す前は無条件に 0041h を読んでいたので、空 IRQ でも前回のバイト (か不定値) を
打鍵として配り、PE/FE の化けたバイトもそのまま配り、エラーフラグは
`kbd_init` 以後一度も解除しなかった。判定を I/O の無い純粋部に切り出して
ここで固定する。

## なぜホストで見るのか

**EMPTY と ERROR は NP21/W では踏めない**。NP21/W の `keyboard_i43` は
`status | 0x85` を返し、IRQ1 を上げる前に必ず RxRDY (bit1) を立てる。エラー
ビットは自前のバッファが溢れたときの OE しか出さない。逆に `| 0x85` で立つ
ビット (DSR / TxEMP / TxRDY) を判定に混ぜると、エミュレータの打鍵が全部落ちる
— これも実機を待たずにここで固定する。

## RED

### RED 1 — 直す前の姿 (無条件に 0041h を読む = 常に DATA)

判定を「常に `KBD_ST_DATA`」にした写しで同じハーネスを回した
(直す前の IRQ1 ハンドラと同じ振る舞い):

```
FAIL case_empty:27: kbd_status_classify(0x00) == KBD_ST_EMPTY
FAIL case_empty:28: kbd_status_classify(0x85) == KBD_ST_EMPTY
...
FAIL case_error:36: kbd_status_classify(0x02 | 0x08) == KBD_ST_ERROR
FAIL case_error:37: kbd_status_classify(0x02 | 0x10) == KBD_ST_ERROR
...
EXIT empty=1
EXIT error=1
EXIT data=0
```

### RED 2 — 変異 (否定側)

下の GREEN の `MUTATION 1〜6` がそれぞれ 1 件以上落ちること。

## GREEN

```
$ python3 -B tools/tests/test_kbd_status.py --target --mutate
HOST GNU89 -Werror compile PASS (real drivers/kbd_status.c)
TARGET i386-elf GNU89 -Werror PASS
EXIT empty=0
EXIT error=0
EXIT data=0
SUMMARY 3/3 PASS
MUTATION 1 RED (1 件): RxRDY を見ない (空 IRQ でも 0041h を読んで前回のバイトを打鍵にする)
MUTATION 2 RED (1 件): エラービットを見ない (化けたバイトを打鍵として配る)
MUTATION 3 RED (1 件): エラーを RxRDY より先に見る (受信データが無いのに 0041h を読み捨てる)
MUTATION 4 RED (3 件): RxRDY を bit2 (TxEMP) と取り違える (NP21/W では常に立つので全部 DATA に化ける)
MUTATION 5 RED (1 件): OE を落とす (オーバーランのバイトを使う)
MUTATION 6 RED (1 件): DSR (bit7) をエラーに数える (NP21/W の `| 0x85` で打鍵が全部落ちる)
```

## 何を見ているか

| ケース | 見るもの |
|---|---|
| `empty` | RxRDY = 0 なら、エラービットが立っていても EMPTY (0041h を読まない)。NP21/W の空 (0x85) も EMPTY |
| `error` | RxRDY = 1 で PE / OE / FE を 1 ビットずつ、NP21/W の溢れ (`0x85 | 0x02 | 0x10`)、全ビット |
| `data` | RxRDY だけ、NP21/W の打鍵 (0x87)、bit0 / bit2 / bit6 / bit7 の 16 通りの組み合わせがどれも DATA |

## ここでは見られないもの (実機)

- コマンド語 0x16 で実機の打鍵が戻るか (DTR = RTY# の極性は資料の記述
  だけが根拠 — 戻らなければ `kbdstat` で切り分ける、§4-57)
- IRQ1 がそもそも来るか (PIC の経路)、キーボードが送っているか
- エラー解除 (ER 込みのコマンド語の書き直し) の後に受信が続くか
