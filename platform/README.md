# `platform/` — 機種依存の実装

CPU (`arch/`) とは別の軸。ポート I/O のように「その CPU なら必ずこう」では
なく、**機器のつなぎ方**で決まるものを置く。

    platform/pc98/platform_io.h     PC-9801/9821 のポート I/O と I/O ウェイト

契約は [`include/io.h`](../include/io.h)、選ぶのは `build/config.mk` の
`PLATFORM ?= pc98` と `-Iplatform/$(PLATFORM)`。

2 本の軸の関係、足し方、番人については [`arch/README.md`](../arch/README.md) を読むこと
(片方だけ読めば済むように、手順はそちらに 1 か所でまとめてある)。
