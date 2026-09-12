# con_sink (K6C) — ホスト TDD の記録

票: [docs/tasks/gui/v13/TASK_K6C_console.md](../../docs/tasks/gui/v13/TASK_K6C_console.md) §2
実装: `kernel/con_sink.c` / `include/con_sink.h` / `sdk/include/os32/os32_kapi_shared.h` の `CON_SINK_*`
試験: `tools/tests/con_sink_host.c` + `tools/tests/test_con_sink.py` (`make check-con-sink-host`)

## 様式

`test_multiapp_model.py` / `test_ext2_read_bound.py` と同じ形で、**模型ではなく実物**を見る:

1. `con_sink_host.c` が `kernel/con_sink.c` を `#include` する (1 行も写さない)。
2. ホスト ILP32 GNU89 (`gcc -m32 -march=i386 -ffreestanding -Werror -nostdlib`) で走らせる。
   libc は使わず、Linux の `int 0x80` で `write`/`exit` するだけ。
3. **同じソース**をカーネルと同じフラグの `i386-elf-gcc -Werror` でもコンパイルする ([C1])。

ホスト側だけ `-DCON_SINK_NO_IRQ_LOCK` を付ける。CPL=3 では `cli` / `popfl` を実行できないので、
`irq_save` / `irq_restore` の錠だけ空に差し替える。守っている不変条件 (「錠の中でしか
`head`/`tail`/`count` を動かさない」) はソースの形が同じで、`io.h` を使う本番の経路は
手順 3 のクロスコンパイルで通る。

`make`・エミュレータ・配備は使わない。

## 試験の区分 (64 チェック)

| 区分 | 何を固定したか |
|---|---|
| 1 disabled | CUI モード中 (無効) は 1 バイトも溜めない — 受入 C1 の `pending == 0` |
| 2 record shape | `PRINT` = `[1][color][len][本文]` / `CLEAR` = `[2]` / `CURSOR` = `[3][x][y]`、長さ 0 と NULL は積まない |
| 3 record boundary | 読みはレコード境界で切る。半端な `cap` でも次のレコードの頭を齧らない。`cap < CON_SINK_REC_MAX` と NULL は `OS32_ERR_INVAL` で、弾いた読みはリングを動かさない |
| 4 print split | 200 バイト超は分割。全角が境界に跨るときは **UTF-8 の切れ目**まで戻す (200 = 3×66+2 なので素朴に切ると 67 文字目が割れる) |
| 5 overflow | 容量内では捨てない。あふれたら**古い方をレコード単位で**捨て、`dropped` が進む。環の折り返しを跨いだレコードもそのまま出る。長さの違うレコードが混ざっても端数は残らない |
| 6 discard on CUI | `con_sink_disable()` で捨てる / 無効中は積まない / 再入場は空から |
| 7 single reader | 最初に読んだ所有者が持つ。2 本目は `OS32_ERR_EXIST` で、そのときリングは動かない。`con_sink_owner_exit(持ち主)` の後だけ次の 1 本が読み手になれる (受入 C4)。CUI 往復では所有は動かない。`stat` は所有が要らない |
| 8 selftest | `con_sink_selftest()` (実機の kselftest がブート時に呼ぶもの) がホストでもビットマスク 0 |

## RED → GREEN

実装を 1 か所ずつ「ありそうな間違い」に差し替えて、試験が**その間違いだけ**を捕まえることを見た。
差し替えは全部戻してある (`kernel/con_sink.c` は GREEN の形)。

### R1 — あふれをバイト単位で捨てる (レコード枠を持たない環)

`ring_drop_oldest()` の `size = ring_rec_size()` を `size = 1` に。

```
  FAIL 5c 捨てた回数が 1 増える
  FAIL 5e 残った先頭は 2 本目 (= 最古が消えた)
  FAIL 5f 折り返しを跨いだレコードもそのまま出る
  FAIL 5h 端数レコードが残らない
  FAIL 5i 先頭は必ずレコードの先頭
  FAIL 8a 自己診断のビットマスクは 0
FAILURES
EXIT con_sink_host=1
```

最初の版 (「満杯なら新しい方を捨てる = `ring_reserve` が 0 を返す」) は
`ring_drop_oldest` が未使用になって `-Werror=unused-function` で**コンパイルが落ちた**ため、
「バイト単位で捨てる」に差し替えた。RED は試験の失敗として出したいので、
ビルドが通る間違いを選ぶ。

### R2 — 読みが `cap` まで詰める (レコード境界を見ない)

`con_sink_read` の `size = ring_rec_size(); if (written + size > cap) break;` を
`size = g_count;` + `cap` へのクリップに。

```
  FAIL 3j 半端な cap でも 1 本ぶんで止まる
  FAIL 3k 2 本目は丸ごと残る (頭だけ齧られない)
  FAIL 3l 2 本目は次の読みで丸ごと出る
  FAIL 5i 先頭は必ずレコードの先頭
FAILURES
EXIT con_sink_host=1
```

**最初の RED では 5i しか落ちなかった**。3a〜3e は `cap` をレコード長ちょうど
(`CON_SINK_REC_MAX`) で呼んでいたので、境界を見ない実装でもたまたま同じ答えになる。
`cap` がレコードの倍数でない形 (3j〜3l) を足してから、この間違いが捕まるようになった。

### R3 — 読み手の所有を見ない

`con_sink_read` の所有判定 (`g_reader` の取得 / `OS32_ERR_EXIST`) を削除。

```
  FAIL 7b 2 本目の読み手は拒否される
  FAIL 7c 拒否された読みはリングを動かさない
  FAIL 7d 持ち主は読み続けられる
  FAIL 7e 別 ID の回収では明け渡さない
  FAIL 7g 今度は元の持ち主の方が拒否される
  FAIL 7h CUI 往復では所有は動かない
  FAIL 8a 自己診断のビットマスクは 0
FAILURES
EXIT con_sink_host=1
```

### R4 + R5 — CUI 復帰で捨てない / UTF-8 の切れ目を見ない

`con_sink_disable()` から `ring_reset()` を外し、`utf8_chunk_len()` を `return max;` に。

```
  FAIL 4e 1 本目は UTF-8 の切れ目 (66 文字 = 198B) まで
  FAIL 4f 2 本目に 3 バイトまるごと残る
  FAIL 4g 2 本目の先頭は先行バイト
  FAIL 6c 溜まっていたものは捨てる
  FAIL 6d 無効中は積まない
  FAIL 8a 自己診断のビットマスクは 0
FAILURES
EXIT con_sink_host=1
```

### GREEN (最終)

```
8 con_sink_selftest (what kselftest runs on the guest)
  ok   8a 自己診断のビットマスクは 0
  ok   8b 終わった後は無効 (CUI に戻る)
  ok   8c 終わった後は空
  ok   8d 所有者タグを元に戻す
ALL PASS
TARGET i386-elf GNU89 -Werror COMPILE PASS
EXIT con_sink_host=0
```

## この試験が言わないこと

- **実機のことは何も言わない。** 割込み文脈からの実際の同時書き込み (IRQ0/IRQ1 が
  `kprintf` する形) はホストでは再現していない。錠は空に差し替えてあり、見ているのは
  「錠の中でしか状態を動かさない」というソースの形だけ。ブート時の
  `con_sink_selftest()` (kselftest 6 ケース) も同じ単一文脈での確認にとどまる。
- KAPI 経由 (CPL=3 の `int 0x80`、ディスパッチャのポインタ検証) は通っていない — 受入 C3。
- `kernel/console.c` の 6 入口からの差し込みが漏れていないかは、ここでは見ていない
  (差し込み先はホストに持ち込めない TVRAM / GDC と混ざっている)。受入 C1 / C2 の担当。
