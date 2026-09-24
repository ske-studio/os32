# FD のトラック単位の読み出し — シークの省略・まとめ読み・先読み (ホスト試験の記録)

- 症状: 実機 **PC-9821Ra266** で 2HD 1232KB の FD から起動すると、とても遅い。
  既定フォント (`/sys/font/default.kcg`、LZ4 で 188KB) の読み込みで 1 分以上止まる。
  NP21/W は回転待ちもシーク時間も模擬しないので、エミュレータでは再現しない
- 見立て (机上、**実機では測っていない**): 旧 `disk_read` (DRV_FDD) は 1 セクタずつ
  `SEEK` + 20ms 待ち + `READ DATA` (R = EOT) を出していた。次のセクタはヘッドの下を
  過ぎているので、1 セクタごとにほぼ 1 回転 (360rpm で約 167ms) 待つ
- **要求を束ねるだけでは足りない**: FatFs は `FF_FS_TINY=1` で、セクタに揃わない読みを
  1 セクタの窓 (count=1) で回す。フォントは 16B のヘッダの後に 1024B ずつ読む
  (`kernel/boot_font.c` の `kcg_read_chunked`) ので、**要求は全部 count=1**。そこで
  要求したセクタからトラックの終わり (EOT = spt) までを 1 回で読み、残りを持っておく
- 対象: [`drivers/fdc_track.c`](../../drivers/fdc_track.c) (区切り・先読み・読み直しの純粋な層)、
  [`drivers/fdc.c`](../../drivers/fdc.c) (`fdc_read_sectors`、覚えたシリンダ)、
  [`drivers/fdc_decide.c`](../../drivers/fdc_decide.c) (`fdc_rw_timeout_ticks`)、
  [`fs/fatfs/diskio.c`](../../fs/fatfs/diskio.c) (結線と、書き込み・再初期化での破棄)
- 実行: `python3 -B tools/tests/test_fdc_track.py [--target] [--mutate] [case ...]`
  (`make check-fdc-track-host` が `--target --mutate` 付きで回す)
- 試験: [`fdc_track_host.c`](fdc_track_host.c) — 実物を `#include` する。
  (1) 偽の fdc (`struct fdc_track_ops`) で区切り方と読み直しを見る。
  (2) **本物の `fdc.c`** を µPD765A の模型の上で回す。ポートは
  [`fdc_hostshim/io.h`](fdc_hostshim/io.h) が模型へ回し、`tick_count` は
  `-Dtick_count=(*fdc_fake_tick_ptr())` で「読むたびに 1 進む」時計にする
- KAPI は動かしていない。既存 API のシグネチャもそのまま。足したのは
  `fdc_read_sectors` / `fdc_get_stats` / `fdc_print_stats` / `fdc_media_gen` /
  `fdc_track_slot` / `fdc_get_known_cyl` (カーネル内だけ)

## 0. 9ed7c80 を NP21/W で起動して分かったこと (2026-09-24 夕) と直したもの

PM の実測: フォントの読み込みは 4a8fad4 とほぼ同じ約 3 分。EIP は全部
`fdc_wait_seek_end` の IRQ 待ち (0x118140〜0x118150 = 内側に展開された `fdc_wait_irq`)。

- **主因 (机上で確定、実測は PM)**: FatFs は `FF_FS_TINY=1` で FAT とデータが 1 つの窓
  (`fs->win`) を取り合う。`f_read` (ff.c 3914〜) はクラスタを越えるたびに `get_fat` →
  `move_window(FAT のセクタ)` で窓を FAT に替え、次のデータのセクタで窓を戻す。フォントは
  16B のヘッダの後に 1024B ずつ読むので**毎回セクタ (= 2HD の 1 クラスタ) を越える**。
  FAT はシリンダ 0、データは奥なので、**1KB ごとにシークが 2 回**。先読みを 1 本しか
  持たないと FAT のトラックとデータのトラックが追い出し合い、まったく当たらない。
  → 先読みを **2 本 (LRU)** にした。`fat_data_interleave` で、同じ読み方が
  READ 6 回・SEEK 4 回・期限切れ 0 になることを本物の `fdc.c` で見る。
- **完了待ちの取りこぼし (実機の形)**: `fdc_wait_seek_end` は SIS が別ドライブの通知や
  Ready 変化を返すと、1 件読んだだけで次の IRQ を待っていた。µPD765A の INT 線は pending が
  尽きるまで上がったままで、エッジの PIC には次のエッジが来ない → 自分の完了が FIFO に
  残ったまま **1.5 秒の期限まで空待ちし、救済の SIS で拾う**。1 本のエッジで 80h が出るまで
  読むようにした (`seek_edge_foreign`)。NP21/W は事象ごとに IRQ を出す
  (`fdc_intdelay` → `fdc_interrupt`) ので、エミュレータでこれが主因かは起動時の
  `[fdc] font: ... tmo=` の行で確かめる
- **IF=0 に見えたもの**: 0x244 は IF (bit9 = 0x200) が立っている (0x200 + ZF 0x40 + PF 0x04)。
  0x214 と同じく IF=1 で、割り込み禁止の区間ではない
- 模型は **INT 線をレベルで持ち、立ち上がりだけ `fdc_irq_fired` を立てる**形にした
  (pending の SIS 結果か、読み終わっていない READ/WRITE のリザルトがあるあいだ上がったまま)。
  これで「省略を排水より先に置く」変異が RED になる (`drain_before_skip`)
- レビューの major:
  - 先読みのスロットに**中身の世代** (`fdc_media_gen`) を持たせた。`fdc_write_sector_geom` を
    通る書き込み全部 (FatFs / dev.c の fd0・fd1 / KAPI の `dev_blk_write`)、SIS で見た Ready
    変化、`fdc_set_media` で進む。diskio.c の `disk_write` での破棄はこれに置き換えた
  - `.bss` の詰め物: 受け皿を **1 本の静的な領域 (27KB = 窓 1 + スロット 2)** にし、DMA の窓の
    位置を `fdc_buf_layout()` が実行時に決める (領域は 64KB より短いので境界は高々 1 本、窓は
    先頭か末尾に必ず取れる)。揃え指定もリンカスクリプトも使わない。`__bss_end` は
    0x184800 → 0x180780
  - diskio.c の破棄の試験 (`diskio_rw`): 読む → `disk_write` → 読む、読む → `fdc_write_sector`
    直 → 読む、Ready 変化の無い入れ替え → `disk_initialize` → 読む。
    `diskio_set_fdd_drive` の破棄は外した — `STA_NOINIT` にするので次の読みの前に必ず
    `disk_initialize` を通り、そこで捨てる (残すと等価な変異になる)
- minor: NR はまとめ読みの失敗に数えず行も出さない (`multi_nr_quiet`、戻り値 -3)。
  Ready 変化は SIS のどこで見ても世代を進め、覚えたシリンダを捨てる
  (`readychange_invalidates`)
- 起動時に `[fdc] font: seek= skip= recal= tmo= foreign= rdy= multi=ok/fail nr= single= retry= write=`
  を 1 行出す (`kernel/kernel.c`、FD に触っていなければ出ない)

## 1. ケース

| ケース | 見るもの |
|---|---|
| `split_2hd` / `split_144` | spt=8/1024B と spt=18/512B で、count=1、トラックの終わりちょうど、ヘッドの境目、シリンダの境目、最後のシリンダ |
| `readahead_count1` | count=1 の連続が **1 トラック 1 コマンド**になる。途中のセクタから始めたらそこから EOT まで。手前は持っていないので読みに行く |
| `cross_boundary` | 境目をまたぐ要求がトラックごとに分かれ、どのコマンドもトラックを越えない。中身が正しく、受け皿の外 (前後 64B) を書かない |
| `fallback_single` | まとめ読みが失敗したら**要求した区間だけ** 1 セクタずつ読み直す (先読みの分は読み直さない)。失敗で壊れた受け皿を前のトラックとして当てない。先読みが落ちたトラックでは以後、要求の範囲だけを束ねて読む (印は破棄で忘れる) |
| `single_fail` | 要求の中に読めないセクタがあれば失敗を返す |
| `cache_rules` | 破棄 / 別ドライブ / 別ジオメトリ / 別ヘッド / 別シリンダでは当てない |
| `oversize_geom` | 受け皿に入らないジオメトリは束ねずに 1 セクタずつ |
| `timeout_math` | まとめ読みの時間上限 (下の §2) |
| `fdc_multi_cmd` | 本物の `fdc.c`: READ DATA が MT=0、R=sect、EOT=sect+count-1、DMA 長 = count×bps、受け皿が 64KB 境界をまたがない ([HW2])、トラックをまたぐ引数は I/O の前に断る |
| `fdc_seek_skip` | 同じシリンダならシークを省く (ヘッドが違っても、単発でも、書き込みでも)。違えばシークし、覚えた値を更新する |
| `fdc_forget_rules` | まとめ読みの失敗 (DMA を閉じ、リセット + RECALIBRATE でヘッドを 0 に戻して 0 を覚え直す)・その RECALIBRATE も落ちたとき・メディアの変更・ドライブの切り替え・単発の最終失敗・IRQ 無し・シークの失敗で覚えた値を捨てる |
| `fat_data_interleave` | FatFs の読み方 (FAT のセクタとデータのセクタが交互) で、2 本なら FAT のトラックは 1 回だけ読む。本物の `fdc.c` で SEEK 4 回・READ 6 回・期限切れ 0 |
| `gen_and_lru` | 世代が進んだスロットは当てない。LRU で使ったばかりのスロットを残す |
| `seek_edge_foreign` | SEEK の完了の前に別ドライブの通知 / 自ドライブの Ready 変化が積まれていても、1 本のエッジで全部読んで期限切れを待たない。Ready 変化で世代が進む |
| `drain_before_skip` | 取り残しの通知で INT 線が上がったままでも、省略の前の排水で下ろし、READ の完了のエッジが来る |
| `readychange_invalidates` | SIS で Ready 変化を見たら、持っている先読みを入れ替え後の媒体に当てない |
| `multi_nr_quiet` | SEEK の NR / READ の NR はまとめ読みの失敗に数えず、行も出さない |
| `diskio_rw` | diskio.c ごと: 初期化前は NOTRDY、同じトラックは 1 回だけ読む、`disk_write` と `fdc_write_sector` 直の後に古い中身を返さない、Ready 変化の無い入れ替えは `disk_initialize` で読み直す |
| `buf_layout` | 受け皿の割り付け: 境界がどこにあっても DMA の窓は 64KB をまたがず、先読みの領域と重ならない |
| `fdc_end_to_end` | `fdc_track_read` + 本物の `fdc.c`: count=1 × 48 セクタが READ 6 回・SEEK 3 回。裏でヘッドが動いていても (V86 の BIOS など) ID 部の照合 (WC) で落ちて読み直し、別のシリンダは読まない |

## 2. 資料の根拠

- **EOT と TC**: 単発の読み (R = EOT、DMA 長 = 1 セクタ) が実機で通っている。
  まとめ読みは同じ組み立てを count セクタに伸ばしたもので、DMA の TC と EOT が
  最後のセクタの最後のバイトで同時に来る。NP21/W は `src/io/fdc.c` の
  `FDC_ReadData` が `FDCEVENT_NEXTDATA` で `R++ == eot` まで `readsector()` を
  繰り返し、`fdc_dmafunc` の `DMAEXT_END` で `tc` を立てる
- **MT は使わない**: PC9800Bible 2-9 の INT 1Bh「データの読みだし (06h)」は
  「MT を指定し開始ヘッドが 0 なら同じシリンダのヘッド 1 も読める」とし、NP21/W も
  `fdc.mt` で `H ^= 1` して続ける。使えば 1 コマンド 1 シリンダ (2HD で 16KB) だが、
  (a) 転送長を 1 トラック以下に保つ (受け皿 9KB)、(b) 同資料は**書き込みの MT を禁じて
  いる** (µPD765 が正しく動かない)、読みの MT を実機で確かめた記録がまだ無い、
  (c) 得はシリンダごとにヘッド 1 の先頭の待ち (最悪 1 回転) を省くだけ、の 3 つで見送った
- **時間上限** (POLICY_DEBUG §4-51 の「機構の最悪値」):
  `2 × (2 回転 + ceil(count/spt) 回転 + HLT 10ms)`、回転は遅い 300rpm の 200ms。
  1 トラック全部で 2 × (400 + 200 + 10) = 1.22 秒。単発の 1 秒を下限にする。
  シークは `fdc_seek` が自分の上限 (1.5 秒) で別に待つ

## 3. 変異 (否定側)

`--mutate` は実装の写しを一時の木に置いて 1 か所ずつ壊す (ソースは書き換えない)。
判定: RED = どれかのケースが落ちた / ERROR = コンパイルできない (**RED に数えない**) /
SURVIVED = 見逃し。最後の 1 本は何も変えない対照で、SURVIVED でなければならない。

2026-09-24 の結果: **RED 20 / ERROR 0 / SURVIVED 1 (対照)** (変異 21 本)。

2026-09-24 夕 (上の §0 を直した後): **RED 32 / ERROR 0 / SURVIVED 1 (対照)** (変異 33 本)。
途中で SURVIVED が 2 本出た: (a) 覚えたシリンダの破棄はリセット・RECALIBRATE に加えて
Ready 変化の SIS でも行うので三重 (3 つとも消す形にして RED)、(b) `disk_initialize` の変異が
スロットの貸し出しごと消していて先読み自体が止まり、古い中身を返しようがなかった
(貸し出しと破棄を分け、試験に「同じトラックは 1 回だけ読む」を足して RED)。

途中で見つけた穴 (直した):
- 「トラックの境目で切らない」の最初の書き方は未使用変数でコンパイルが落ちた (ERROR)。
  境目を 1 つ越えて切る形に書き直して RED
- 「まとめ読みの前に捨てない」が SURVIVED — 偽の fdc が失敗のとき受け皿を壊して
  いなかった。実行フェーズの途中で落ちた DMA の姿 (受け皿を途中まで壊す) にして RED
- 「覚えた値を捨てる」は二重になっている所がある: DMA を積んだ後の失敗はリセット
  (`fdc_forget_all`) と RECALIBRATE の出す前の破棄、シークの失敗は `fdc_seek` の出す前の
  破棄とまとめ読みの失敗経路の破棄。片方だけ消す変異は振る舞いが変わらない (等価な変異)
  ので SURVIVED になった。両方消す形にして RED
- 自己レビューで 2 点足した: (1) まとめ読みの失敗の後にリセットだけでなく RECALIBRATE も
  出す (リセット後の PCN を信じて単発が別のシリンダへシークし、WC で 1 回無駄にしないため。
  単発のリトライの間の `fdc_recover` と同じ形)、(2) 先読みが落ちたトラックに印を付け、
  以後そこでは要求の範囲だけを読む (要求の外の傷で毎回「失敗 + 回復 + 1 セクタずつ」を
  踏まないため)。どちらも変異を足して RED を確かめた

## 4. ホストで見ていないもの

- diskio.c はホストでも回す (`diskio_rw`)。FatFs 本体 (ff.c) は通していない
- 実機の速度、エミュレータでの起動、実機での MT なしのまとめ読みの挙動 (TC と EOT が
  同時に来て正常終了すること) は未確認
