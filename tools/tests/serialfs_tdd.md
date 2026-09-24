# serialfs 検証記録 (シリアル越しの /host、`sfs run`)

票: [TASK_SERIAL_HOSTFS](../../docs/tasks/realhw/TASK_SERIAL_HOSTFS.md) 部品 B
(§1-v2 / §1-v3 / ユーザー決裁 2026-09-24)。受入 T1 と T5 のホスト部分。

## この文書の性格

実装を先に書き、試験は後から足した。RED の根拠は**変異がすべて RED になること**
(下の表) で、変更前のコードで試験を回した RED→GREEN の経過ではない ([V4])。
試験を書く途中で本物の欠陥を 1 つ見つけた: `fs/serialfs.c` がホストの status を
`(i32)` で読んでおり、ホスト試験の 64 ビット環境では負の値が正に化けて
「知らない status を IO に畳む」が効かなかった (i386 では 32 ビットなので起きない)。
`int` で読むように直した。

## 実行

```
python3 -B tools/tests/test_serialfs.py --target --mutate   # make check-serialfs-host
```

| 段 | 何を | どうやって |
|---|---|---|
| C | フレーム・CRC32・番号・再送・長さの境界・VFS の契約・障害の注入 | `serialfs_host.c` が実物の `fs/sfs_proto.c` / `fs/sfs_client.c` / `fs/serialfs.c` を取り込み、偽の線 (仮想の tick、届く時刻つきのバイト) と C の偽ホストで回す |
| GATE | ゲート (呼び口で分ける) | `serial_gate_host.c` が実物の `drivers/serial.c` を偽のポートで回す |
| PY | ホスト側の振り分け・パス・キャッシュ・期限・BYE・行の外 | 実物の `tools/serialfs_host.py` と `tools/rshell_serial.py` を偽のポートで |
| PY | NP21/W の COM1 を HTTP で読む口 (`--port aidebug:...`) | 偽の urlopen で `/api/serial/read`・`write` の分割・空の回・読み捨て |
| XC | C ⇔ Python | C の組んだ列を Python が読む / 逆、定数の照合、**実時間のパイプで結合** (RENAME の応答を 1 回落とし、READ の応答の CRC を 1 回壊す) |

障害の注入 (T5): 応答の喪失 (1〜3 回は通る / 4 回で IO / 3 要求続けて死ぬ)、遅延
(遅れた応答が次の要求の最中に届いても取り違えない)、CRC 破損、番号違い・セッション
違いのフレーム (数えずに捨てて期限を延ばさない — 流れ続けても抜ける)、ホストの停止、
ごみの連続、ERR (未知のセッション)、契約違反の応答 (要求より多い READ・`/` を含む名前・
進まない cookie・知らない status)、再送で副作用が二重にならない (ホストのキャッシュ、
結合でも)、セッション中に rshell へフレームが漏れない (ゲート)、`cat` の本文にフレームが
あってもセッション外では答えない。

`hsync` の vmkernel.old の門は、純粋な判定 (`hsync_bootold.inc`) をこの試験で、
実物の hsync.c を通した流れ (一致 → BACKUP してから置き換え / 不一致・記録なし →
置き換えない / `--no-backup` / dry-run / 同じ内容なら作り直さない) を
`tools/tests/hsync_h2_host.c` の `case_boot_old` (make check-hsync-h2-host) で見る。
既存の `/boot` の案件 (rename 後の sync 失敗) は贋カーネルが v53 で起動イメージを
答えないので `--no-backup` を足した。

## 変異

組めない・当てはまらない変異は ERROR として数える。恒等の対照を C と Python に 1 本ずつ
置き、それが GREEN であることを見る。変異の組み立ては `-Werror` を外す (使われなくなった
引数の警告で ERROR にしない)。実物の組み立ては `-Werror`。

2026-09-25: 51 本 (C 33 + ゲート 6 + Python 12)、うち対照 2 本 GREEN、残り 49 本すべて RED、
ERROR 0。一覧は `test_serialfs.py` の `C_MUTATIONS` / `GATE_MUTATIONS` / `PY_MUTATIONS`。

## 試していないこと

- NP21/W と実機 (T2 / T3 / T4)。**NP21/W は通信速度を模擬しない**ので、115200 の
  16 バイト FIFO の取りこぼし・ISR が汲み残す FIFO の末尾は実機でしか踏めない
- カーネルの `fs/serialfs_session.c` (ゲート・HELLO・マウント・隔離・ログのフレーム) は
  i386-elf で組むだけで、ホストでは回していない (KAPI・VFS・tick に依存する)
- 常駐シェルの `sfs run` と rshell の行の読み取り (ESC の単独判定の待ち) は、純粋な
  判定 (`rsh_esc_classify` / `rsh_sfs_child`) だけを回している
