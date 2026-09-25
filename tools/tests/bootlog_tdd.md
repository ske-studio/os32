# 起動ログ (/var/log/boot.log) — ホスト TDD の記録

票: 実機 PC-9821 で起動画面の `[selftest] N/N passed` などがすぐ流れて読めない件 (ユーザー提案、2026-09-25)。
rshell が立つ前の kprintf はシリアルにも出ない (`kernel/console.c` は `rshell_active` のときだけ写す)。
実装: `kernel/bootlog.c` (純粋な部分) / `kernel/bootlog_save.c` (VFS と版情報への結線) / `include/bootlog.h` / `include/config.h` の `SYS_BOOTLOG_*`
試験: `tools/tests/bootlog_host.c` + `tools/tests/test_bootlog.py` (`make check-bootlog-host`)

## 様式

`test_kprintf_attr.py` / `test_con_sink.py` と同じ形で、**模型ではなく実物**を見る:

1. `bootlog_host.c` が `kernel/bootlog.c` を `#include` する (1 行も写さない)。
2. ホストの gcc `-std=gnu89 -Wall -Wextra -Werror` で走らせる。ホスト側だけ
   `-DBOOTLOG_NO_IRQ_LOCK` (CPL=3 では `cli` / `popfl` を実行できない)。
3. **同じソース**と結線 (`kernel/bootlog_save.c`) を、カーネルと同じフラグ + `-Wextra -Werror`
   の i386-elf-gcc でもコンパイルする (`--target`)。本番の `irq_save` / `irq_restore` の形はここで通る。
4. 書き出しの手順は `BootlogFsOps` 越しなので、偽の VFS (呼ばれた順を記録し、段ごとに失敗を注入。
   FAT 版は `rename` が宛先ありで `OS32_ERR_EXIST` = FatFs の `f_rename`) で回す。
5. 結線の静的検査: `console.c` の入口 4 か所 (con_sink を積む所と同じ) で積むこと、`kernel.c` で
   ルートのマウントと `kselftest_run` の後・`exec_run(cur_shell)` の前に 1 回だけ呼ぶこと、
   `bootlog_save` が最初に止めること (自分の kprintf を溜めない)。

`make`・エミュレータ・配備は使わない。

## 試験の区分 (ホスト 62 チェック + 結線 3)

| 区分 | 何を固定したか |
|---|---|
| 1 collect | 最初から積む (BSS のまま)、NULL と長さ 0 は積まない、順番どおり |
| 2 overflow | ちょうど満杯は捨てない / 満杯の後は**新しい方を**捨てて数える / 入る分だけ写して残りを数える / 本文の外を踏まない |
| 3 utf8_latch | あふれの境目で UTF-8 の文字を割らない (割らずに捨てた分も数える)。**一度あふれたら**空きが残っていても後の行は捨てる (途中が抜けた本文にしない) |
| 4 stop | 止めた後は積まない、「捨てた」にも数えない |
| 5 header | ヘッダの全文 (CRC は 8 桁の 16 進)、CRC 無しは `none`、Build 無しは `?`、切り詰めても改行 + NUL で終わる |
| 6 compose | ヘッダ + 本文 + 末尾行が連続した 1 本 / 何度組んでも同じ / 行の途中で終わる本文には改行を足す (本文には入れない) / 空でも 2 行出る / あふれても末尾行は置き場に収まる |
| 7 plan | ext2 と fat だけ書く (hostdrv / iso9660 / serialfs / 未マウントは書かない、名前は完全一致)。FAT の名前は全部 8.3 (FatFs `create_name` と同じ判定)、ext2 の前回分は `boot.log.1` |
| 8 save | 順序 mkdir → mkdir → rm .1 → rename → write → sync。2 回目以降の回し方 (ext2 / FAT)。`/var`・`/var/log` が作れなければ書かずに止める。rm が落ちたら付け替えずに書く。rename が落ちても書く。write の失敗・書いた量の不足は WRITE で sync しない。sync の失敗。最初の失敗の段と rc を返す。書かない種別は何も呼ばない |

## 否定側 (`--mutate`)

実装を 1 か所ずつ「ありそうな間違い」に差し替え、試験がそれを捕まえることを見る。
**組めない変異は数えない** (コンパイルが落ちたら `SKIP` と出して分母から外す)。
先頭に**恒等の対照** (何も変えない置換) を置き、これは GREEN でなければならない —
RED なら変異の仕組みそのもの (写しの置き場・インクルード順) が壊れている。

最初の版の 9 番 (`bl_put_hex8` → `bl_put_dec`) は `bl_put_hex8` が未使用になって
`-Werror=unused-function` で組めず SKIP になったので、組める間違い (上位桁を落とす) に替えた。

2026-09-25 の結果:

```
MUTATION 0 GREEN (対照) (0 件): 恒等の対照 (何も変えない)
MUTATION 1 RED (1 件): 一度あふれても後から来た短い行を積む (途中が抜けた本文になる)
MUTATION 2 RED (1 件): あふれの境目で UTF-8 の文字を割る
MUTATION 3 RED (2 件): 境目で捨てたバイト数を数え違える
MUTATION 4 RED (1 件): あふれた後に捨てた分を数えない
MUTATION 5 RED (1 件): 容量を 1 バイト踏み越える (末尾行の置き場を壊す)
MUTATION 6 RED (1 件): 止めても積み続ける (書き出しの後の出力が混ざる)
MUTATION 7 RED (1 件): 本文が行の途中で終わると末尾行がくっつく
MUTATION 8 RED (1 件): ヘッダが改行で終わらない
MUTATION 9 RED (1 件): Image CRC の上位桁を落とす (ver の %08x と突き合わせられない)
MUTATION 10 RED (2 件): FAT でも boot.log.1 (8.3 でない名前) を使う
MUTATION 11 RED (1 件): HostDrv / iso9660 ルートにも書く
MUTATION 12 RED (1 件): FAT (FD 起動) では書かない
MUTATION 13 RED (2 件): 既にある /var を失敗と読む (2 回目の起動から書けない)
MUTATION 14 RED (1 件): /var が作れなくても続ける
MUTATION 15 RED (1 件): /var/log が作れなくても続ける
MUTATION 16 RED (1 件): 前回分を消さずに付け替える (FAT の rename が EXIST で落ちる)
MUTATION 17 RED (1 件): 前回分を消せなくても付け替える
MUTATION 18 RED (1 件): 付け替えに失敗したら今回分を書かずに止める
MUTATION 19 RED (1 件): 書いた量の不足を見逃す
MUTATION 20 RED (1 件): sync しない (電源断で消える)
MUTATION 21 RED (1 件): 後の失敗で最初の失敗を上書きする
MUTATION 22 RED (1 件): 付け替えの向きが逆
MUTATIONS 22/22 RED (組めずに除外 0)
```

## ブート時 (kselftest)

`kernel/kselftest.c` の `test_bootlog` (4 項目): 書き出しの前なので**溜まっている最中の本物**を見る —
まだ積んでいる / 本文が空でない / ヘッダが `# OS32 boot log ` で始まり改行で終わる /
compose の結果がヘッダで始まり、本文より長く、改行で終わる。compose は本文を動かさないので
自己試験から呼んでも起動ログは変わらない。

## 試験していないこと

- 本物の ext2 / FatFs への書き出し (NP21/W と実機で PM が見る)。FatFs の `/var` `/var/log` の
  作成は、`create_name` (FF_USE_LFN 0) の規則を読んで 8.3 に収まることを確かめただけ。
- ISR からの同時書き込みの競合 (錠は `kernel/con_sink.c` と同じ `irq_save` / `irq_restore` の形)。
