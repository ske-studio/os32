# v1.3 メモリ予算 — ホスト側測定

状態: ホストビルドの静的測定。ゲスト空き容量・ピーク使用量・断片化は未測定。
親: [PLAN.md](PLAN.md)、判定条件: [REVIEW_T0_T1.md](REVIEW_T0_T1.md) R3。

## 測定対象

`make gshell` は成功。現行ソースからRust releaseビルドと再リンクを実行した。
clean/full buildは行っていない。Cライブラリは既存のMake依存関係で使用した。
配備・エミュレータ操作は未実施。

- ELF: `userland/gshell.elf`
- ELF SHA-256: `8d247c9e3916f33bae6cebadf6181de66dd3e2579622a3eb122d65c452853a69`
- BIN SHA-256: `0c447320650e208d06c86f900764bb1f82c8793c83f13d3b56105d1d69a89409`
- linker警告: libc_a-sbrkr.oのGNU-stack note欠如、およびLOAD segmentのRWX属性。
  ビルドはexit 0だが、警告なしとは扱わない。

## 結果

`i386-elf-size` と `i386-elf-nm -n` で測定した。

| 項目 | 値 |
|---|---:|
| ELF text | 103206 B |
| ELF data | 49796 B |
| ELF bss | 17924 B |
| __bss_start | 0x3255e0 |
| __bss_end / _end | 0x329be4 |
| MEM_SHELL_GUARD | 0x375000 |
| _endからguardまでの静的差 | 308252 B |
| shell exec_heapの定義容量 | 524288 B |

帯域定義は `include/memmap.h:242-248`。リンカ配置は `sdk/link/app_sys.ld`。
308252 Bはnewlib sbrk等と競合する空間の上限差であり、端末専用の利用可能量ではない。
exec_heapは別帯域だが、524288 Bは総容量であり実行時の空きではない。

## T1案の算術評価 (採用未決)

| 項目 | 計算結果 |
|---|---:|
| stdin 4KiB + stdout/stderr各16KiB | 36864 B |
| 各8192セル × 8B × 2ストリーム | 131072 B |
| 上記合計 | 167936 B |

この合計には管理情報、アロケータヘッダ、追加コード、行索引、番兵等を含まない。
ホスト上の計算が帯域内でも、同時稼働時の確保成功は保証できない。

T4のCellはhost試験でsize 8B / align 4Bと確認（x86_64-unknown-linux-gnu）。
`userland/libos32term/tests/model.rs` の `cell_host_layout_measurement` が根拠。
上の8B仮定とは一致するが、guest実寸・Rust ABI保証・ゲスト確保成功を意味しない。

## PM判断

- pipe案は使用時にkernel kmallocを消費する (`fs/pipe_buffer.c:30-46`) ため、無償の予約領域として採らない。
- 初期候補は親exec_heapから実行前に確保し、子実行中には確保も解放もしない方式。
  ただし動的空き・cleanup・失敗経路を検証するまで確定しない。
- セルモデルの単独ホスト試験は実行統合と分離する。モデルの必要容量を計算可能にし、
  提供容量不足をエラーで返す設計なら、ゲストの容量決定前に境界条件を検証できる。
- 起動前のバッファ確保が途中で失敗した場合は全体を巻き戻し、子を起動しない。
- ゲストで親ヒープ空き・ピーク・連続実行後の回収を測る試験は未実施。
