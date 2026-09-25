# 起動ログ (/var/log/boot.log) — ホスト TDD の記録

票: 実機 PC-9821 で起動画面の `[selftest] N/N passed` などがすぐ流れて読めない件 (ユーザー提案、2026-09-25)。
rshell が立つ前の kprintf はシリアルにも出ない (`kernel/console.c` は `rshell_active` のときだけ写す)。
実装: `kernel/bootlog.c` (純粋な部分) / `kernel/bootlog_save.c` (VFS と版情報への結線) / `include/bootlog.h` / `include/config.h` の `SYS_BOOTLOG_*`
試験: `tools/tests/bootlog_host.c` + `tools/tests/test_bootlog.py` (`make check-bootlog-host`)、
実物の FatFs 結線は `tools/tests/fatfs_stat_host.c` (`make check-vfs-mount-dev-host` の `test_fatfs_stat.py`)

## Codex レビュー (Request changes、2026-09-25) で直したこと

| 指摘 | 直し方 |
|---|---|
| [P1] 世代の更新に失敗した後で boot.log を上書きすると既存のログを失う (`.1` 削除 → rename 失敗 → boot.log 切り詰め → write 失敗で両方消える。`.1` だけが残っているときも先に消す) | **今回分をまず一時ファイル `/var/log/boot.new` (8.3) に書き**、write + close が通ったと確かめてから `.1` を消す → boot.log → `.1` → boot.new → boot.log。どの段で落ちてもそこで止め、boot.log は上書きしない (今回分は boot.new に残る)。残った boot.new は次の起動の書き込みで上書きされる (誰も読まない。「残っている = その起動の保存が世代の更新の途中で止まった」)。write が落ちたときは途中で切れた boot.new を消す (成否は問わない) ので、残る boot.new は完全な 1 本だけ |
| [P2] ext2 で正常に保存しても「書き込み不足」(ext2 の `vfs_write` は成功で 0、FAT は書いたバイト数) | `BootlogFsOps.write` の約束を「`vfs_write` の生の戻り (FS ごと)」と明記し、種別を知る手順の側 (`bootlog_write_ok(kind, rc, len)`) で揃える。偽の VFS も ext2 なら 0、FAT ならバイト数を返す。`test_bootlog.py` の `CONTRACT` 検査が実物の `ext2_vfs_write` / `fatfs_vfs_write` の形 (ext2 は `ext2_to_vfs_err(...)`、FAT は `return (int)bw;` と `f_close` の伝播) を見張る |
| [P2] FAT の最後のフラッシュ (`f_close`) の失敗を捨てて保存成功と表示 | `fs/fatfs_vfs.c` の `fatfs_vfs_write` だけ `f_close` の結果を返す (`f_write` の失敗が先)。`write_file` の他の呼び手は `fs/vfs_fd.c` の空ファイル作成と O_TRUNC の 2 か所で、どちらも `rc < 0` を見て返すだけなので、閉じる失敗が「成功した空ファイル」から「失敗」に変わる方向の影響しかない |
| [P2] push をまたぐ UTF-8 を容量の境界で割る (残り 1 バイトに E3 が収まり、次の 81 82 が捨てられる。console.c の `kputc` は 1 バイトずつ積む) | あふれた瞬間に**蓄えた本文の末尾**を文字の境界まで戻し (`bl_trim_partial_tail`)、戻した分を dropped に足す。あふれていないときは戻さない (続きが来る) |
| 試験の穴 | 切り詰めた後の失敗 (8i)、再起動をまたいで唯一の旧ログが残ること (10 reboots)、実物の FAT の close の失敗 (`fatfs_stat_host.c` 12〜14)、IF の復元 (偽の錠で回数・順序・札、3' lock) |

## 様式

`test_kprintf_attr.py` / `test_con_sink.py` と同じ形で、**模型ではなく実物**を見る:

1. `bootlog_host.c` が `kernel/bootlog.c` を `#include` する (1 行も写さない)。
2. ホストの gcc `-std=gnu89 -Wall -Wextra -Werror` で走らせる。ホスト側だけ
   `-DBOOTLOG_NO_IRQ_LOCK` (CPL=3 では `cli` / `popfl` を実行できない) — 錠は試験側が用意し、
   呼んだ回数・順序 (入れ子なし)・`lock` が返した札を `unlock` がそのまま受け取ること (IF の復元) を数える。
3. **同じソース**と結線 (`kernel/bootlog_save.c`) を、カーネルと同じフラグ + `-Wextra -Werror`
   の i386-elf-gcc でもコンパイルする (`--target`)。本番の `irq_save` / `irq_restore` の形はここで通る。
4. 書き出しの手順は `BootlogFsOps` 越しなので、偽の VFS (呼ばれた順を記録し、段ごと・パスごとに失敗を注入。
   FAT 版は `rename` が宛先ありで `OS32_ERR_EXIST` = FatFs の `f_rename`、ext2 版は宛先を置き換える =
   `ext2_rename`。`write` は開いた時点で切り詰め、戻りは ext2 が 0・FAT がバイト数 = 実物の `vfs_write`) で回す。
   実物の `fatfs_vfs_write` (贋物の `f_*`) に結んだ手順は `fatfs_stat_host.c` のケース 14。
5. 結線の静的検査: `console.c` の入口 4 か所 (con_sink を積む所と同じ) で積むこと、`kernel.c` で
   ルートのマウントと `kselftest_run` の後・`exec_run(cur_shell)` の前に 1 回だけ呼ぶこと、
   `bootlog_save` が最初に止めること (自分の kprintf を溜めない)。

`make`・エミュレータ・配備は使わない。

## 試験の区分 (ホスト 105 チェック + 結線 3 + 約束 3、FAT 結線 3 ケース)

| 区分 | 何を固定したか |
|---|---|
| 1 collect (4) | 最初から積む (BSS のまま)、NULL と長さ 0 は積まない、順番どおり |
| 2 overflow (7) | ちょうど満杯は捨てない / 満杯の後は**新しい方を**捨てて数える / 入る分だけ写して残りを数える / 本文の外を踏まない |
| 3 utf8_latch (15) | あふれの境目で UTF-8 の文字を割らない (割らずに捨てた分も数える)。**push をまたぐ文字** (残り 1 バイトの E3 + 次の 81 82、1 バイトずつの 3 バイト文字、4 バイト文字) は蓄えた末尾も戻して dropped に足す。完結した文字・頭の無い継続バイトは触らない。あふれていなければ途中の文字も戻さない。**一度あふれたら**空きが残っていても後の行は捨てる |
| 3' lock (7) | push / compose / stop は錠を 1 回掛けて同じ札で 1 回戻す (あふれ・満杯の経路も)。NULL・長さ 0・止めた後の push と読むだけの API は掛けない |
| 4 stop (3) | 止めた後は積まない、「捨てた」にも数えない |
| 5 header (5) | ヘッダの全文 (CRC は 8 桁の 16 進)、CRC 無しは `none`、Build 無しは `?`、切り詰めても改行 + NUL で終わる |
| 6 compose (7) | ヘッダ + 本文 + 末尾行が連続した 1 本 / 何度組んでも同じ / 行の途中で終わる本文には改行を足す (本文には入れない) / 空でも 2 行出る / あふれても末尾行は置き場に収まる |
| 7 plan (12) | ext2 と fat だけ書く (hostdrv / iso9660 / serialfs / 未マウントは書かない、名前は完全一致)。FAT の名前は全部 8.3 (FatFs `create_name` と同じ判定)、ext2 の前回分は `boot.log.1` |
| 8 save (26) | 順序 mkdir → mkdir → write boot.new → rm .1 → boot.log→.1 → boot.new→boot.log → sync。2 回目以降 (ext2 / FAT)。`/var`・`/var/log` が作れなければ書かない。**write が落ちたら boot.log と .1 は無傷** (切り詰めた後の失敗)、途中の boot.new は消す、世代を動かさず sync しない。rm / boot.log→.1 / 公開のどれが落ちても boot.log を上書きせず今回分は boot.new に残る。FAT の書いた量の不足。sync の失敗。残っていた boot.new は上書きされ公開後に残らない。書かない種別は何も呼ばない。段の名前。boot.new は 8.3 |
| 8' save_mkdir (3) | `/var/log` だけ作れない、`fail_rc` が NULL でも落ちない (通る時も途中で止まる時も) |
| 9 write_rc (7) | `bootlog_write_ok`: ext2 は 0 だけ成功、FAT は書いた量 = len だけ成功 (0 も不足も負も失敗)、書かない種別に成功は無い |
| 10 reboots (9) | 同じ偽 FS に続けて保存: 初回 → write 失敗 (唯一の旧ログが残る) → rm 失敗 (残る、今回分は boot.new) → 公開失敗 (旧は .1、今回分は boot.new) → 正常 (.1 を消すのは今回分を書けた後) → .1 だけの状態で write 失敗 (.1 は残る) → 正常 2 回で 2 世代 |
| FAT 12〜14 (`fatfs_stat_host.c`) | `f_close` だけが落ちると `fatfs_vfs_write` は IO (正常はバイト数、満杯は bw)。`f_write` の失敗が先で `f_close` は 1 回だけ、`f_open` が落ちたら閉じない。実物の `fatfs_vfs_*` に結んだ `bootlog_save_with` は close の失敗で WRITE (rc = IO) で止まり、rename 0 回、unlink は boot.new だけ。対照: 通れば unlink 1 (.1)、rename 2 |

結線 (`WIRING`): console の入口 4 か所、kernel.c の位置、`bootlog_save` が最初に止める、本物の ops は `vfs_*` を生のまま差す。
約束 (`CONTRACT`): 実物の `ext2_vfs_write` が `ext2_to_vfs_err(...)` を返す形、`fatfs_vfs_write` が `(int)bw` を返し `f_close` の失敗を返す形。

## 否定側 (`--mutate`)

実装を 1 か所ずつ「ありそうな間違い」に差し替え、試験がそれを捕まえることを見る。
**組めない変異は数えない** (コンパイルが落ちたら `SKIP` と出して分母から外す)。
先頭に**恒等の対照** (何も変えない置換) を置き、これは GREEN でなければならない —
RED なら変異の仕組みそのもの (写しの置き場・インクルード順) が壊れている。

最初の版の 9 番 (`bl_put_hex8` → `bl_put_dec`) は `bl_put_hex8` が未使用になって
`-Werror=unused-function` で組めず SKIP になったので、組める間違い (上位桁を落とす) に替えた。
レビュー後の版でも同じ理由で 3 つが SKIP になったので、組める形に替えた (末尾を戻さない → `if (0)`、
違う札 → `f ^ 1u`、書いた量を見ない → `<= len`)。「ext2 の負を成功と読む」は `rc < 0` の前置き検査が
残っていて等価な変異だったので、前置きを消して式そのものに負が入らないようにした。
「公開の NOTFOUND を成功と読む」は書けた直後の boot.new が無いという到達しない状況なので外した。

2026-09-25 (レビュー後) の結果:

```
MUTATION 0 GREEN (対照) (0 件): 恒等の対照 (何も変えない)
MUTATION 1 RED (1 件): 一度あふれても後から来た短い行を積む (途中が抜けた本文になる)
MUTATION 2 RED (1 件): あふれの境目で UTF-8 の文字を割る (末尾を戻さない)
MUTATION 3 RED (1 件): 戻すときに文字の頭 (先頭バイト) を残す
MUTATION 4 RED (3 件): 完結している文字まで戻す
MUTATION 5 RED (2 件): 境目で捨てたバイト数を数え違える
MUTATION 6 RED (1 件): あふれた後に捨てた分を数えない
MUTATION 7 RED (2 件): 容量を 1 バイト踏み越える (末尾行の置き場を壊す)
MUTATION 8 RED (2 件): 止めても積み続ける (書き出しの後の出力が混ざる)
MUTATION 9 RED (1 件): 満杯の後の経路で錠を戻さない (IF=0 のまま帰る)
MUTATION 10 RED (1 件): compose が違う札で戻す (退避した IF を復元しない)
MUTATION 11 RED (1 件): 本文が行の途中で終わると末尾行がくっつく
MUTATION 12 RED (1 件): ヘッダが改行で終わらない
MUTATION 13 RED (1 件): Image CRC の上位桁を落とす (ver の %08x と突き合わせられない)
MUTATION 14 RED (2 件): FAT でも boot.log.1 (8.3 でない名前) を使う
MUTATION 15 RED (1 件): HostDrv / iso9660 ルートにも書く
MUTATION 16 RED (1 件): FAT (FD 起動) では書かない
MUTATION 17 RED (3 件): 既にある /var を失敗と読む (2 回目の起動から書けない)
MUTATION 18 RED (1 件): /var が作れなくても続ける
MUTATION 19 RED (1 件): /var/log が作れなくても続ける
MUTATION 20 RED (3 件): 一時ファイルを使わず boot.log に直接書く (切り詰めた後の失敗で失う)
MUTATION 21 RED (1 件): 書いた量の不足を見逃す (FAT の満杯)
MUTATION 22 RED (3 件): ext2 の失敗 (負) を成功と読む
MUTATION 23 RED (2 件): FAT の書いた量の不足を成功と読む
MUTATION 24 RED (2 件): 途中で切れた boot.new を残す
MUTATION 25 RED (2 件): 前回分を消さずに付け替える (FAT の rename が EXIST で落ちる)
MUTATION 26 RED (2 件): 前回分を消せなくても付け替える
MUTATION 27 RED (1 件): boot.log → .1 に失敗しても公開する (boot.log を上書きする)
MUTATION 28 RED (3 件): 公開に失敗しても sync して成功にする
MUTATION 29 RED (1 件): sync しない (電源断で消える)
MUTATION 30 RED (2 件): 付け替えの向きが逆
MUTATIONS 30/30 RED (組めずに除外 0)
```

## ブート時 (kselftest)

`kernel/kselftest.c` の `test_bootlog` (4 項目): 書き出しの前なので**溜まっている最中の本物**を見る —
まだ積んでいる / 本文が空でない / ヘッダが `# OS32 boot log ` で始まり改行で終わる /
compose の結果がヘッダで始まり、本文より長く、改行で終わる。compose は本文を動かさないので
自己試験から呼んでも起動ログは変わらない。

## 試験していないこと

- 本物の ext2 / FatFs への書き出し (NP21/W と実機で PM が見る)。FatFs の `/var` `/var/log` の
  作成は、`create_name` (FF_USE_LFN 0) の規則を読んで 8.3 に収まることを確かめただけ。
- ISR からの同時書き込みの競合 (錠は `kernel/con_sink.c` と同じ `irq_save` / `irq_restore` の形。
  ホストで見るのは回数・順序・札の復元だけで、`cli` が本当に効いているかは見ない)。
- 本物の `f_close` が落ちる状況 (FatFs の中は贋物)。`fatfs_vfs_write` が結果を返すところまで。
