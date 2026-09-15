# `arch/` — CPU 依存の実装

OS32 の移植点は **2 本の軸**に分かれている。

| ディレクトリ | 軸 | 何が入るか | 選ぶ変数 (既定) |
|---|---|---|---|
| `arch/<arch>/` | CPU | 割り込みの許可/禁止・保存/復元、CPU 停止、記述子表のロード | `ARCH` (`x86`) |
| `platform/<platform>/` | 機種 | ポート I/O、I/O ウェイト、機器のつなぎ方 | `PLATFORM` (`pc98`) |

契約 (どの実装も満たすべき宣言と註) は [`include/io.h`](../include/io.h) にあり、
実装はそこに無い。`include/io.h` は末尾で **固定名**を引く:

    #include "arch_io.h"        /* arch/$(ARCH)/arch_io.h     */
    #include "platform_io.h"    /* platform/$(PLATFORM)/platform_io.h */

どの実装が来るかを決めるのは `build/config.mk` の

    ARCH     ?= x86
    PLATFORM ?= pc98
    INC_COMMON = -I. -Iinclude -Iarch/$(ARCH) -Iplatform/$(PLATFORM) $(SDK_INC)

の 2 本の `-I` だけ。だから**アーキテクチャを足す作業は「ディレクトリを 1 つ
足す」で閉じ、既存のファイルには触らない**。`#ifdef __i386__` のような分岐を
本文に増やさないのがこの構成の目的で、`#ifdef` で 1 ファイルの中を分ける形に
戻してはいけない。

なお `arch/x86/arch_io.h` と `platform/pc98/platform_io.h` の**ファイル名が
契約側の `io.h` と違う**のは意図的。同名にすると `-I` の順で実装側が契約
ヘッダ自身を引き込む (あるいはその逆) 事故が起きる。

## 新しいアーキテクチャを足す手順

1. **ディレクトリを作る。** `arch/<arch>/` を掘り、`arch_io.h` を置く。
   既存の `arch/x86/arch_io.h` はコピー元ではなく**対照表**として使う
   — 写すのは命令列ではなく、`include/io.h` の註に書かれた契約のほう。

2. **契約を満たす。** `include/io.h` が宣言している原始命令を全部、同じ
   シグネチャで実装する。宣言と型が食い違えばコンパイルが止まるので、
   取りこぼしは黙って通らない。とくに次の 2 つは命令を並べるだけでは
   満たせない:

   - `_idle()` は「割り込みを許可する」と「眠る」が**不可分**であること。
     2 つの命令に分けて書いてよいかは CPU ごとに違う (ARMv7/v8 なら
     `cpsie i` / `msr daifclr` + `wfi` を、wfi の wake-up event 保持が
     窓を閉じることを確かめたうえで並べる)。
   - `irq_save()` の戻り値は**不透明**であること。呼び手は中身を読まない
     約束なので、EFLAGS でも PRIMASK でも `DAIF` でもよい。

3. **機種側も要るなら `platform/<platform>/` を足す。** ポート I/O を持たない
   CPU では `inp`/`outp` は memory-mapped I/O (レジスタ番地への読み書き) に
   なる。CPU と機種は独立に選べるので、`arch/` を足すたびに `platform/` を
   足す必要はない (逆もまた同じ)。

4. **選ばせる。** `make ARCH=<arch> PLATFORM=<platform> ...`。既定値は
   `build/config.mk` の `?=` なので、コマンドラインでも環境変数でも上書きできる。

5. **計る。** 移植度は合否ではなく数字で見る:

       python3 tools/check_arm_compile.py        # または make check-arm-compile

   基準値と経過は [`docs/tasks/portability/ARM_GAUGE.md`](../docs/tasks/portability/ARM_GAUGE.md)。
   この計測器は `ARCH=x86` の木を ARM コンパイラに通すもので、`arch/arm/` が
   出来ていなくても動く (だから受け皿だけ先に作れる)。

## 番人

`tools/check_arch_asm.py` (`make check` の `check-arch-asm`) が、カーネル側の
C ソースに `hlt` / `cli` / `sti` の直書きが無いことを検査する。直書きしてよいのは
`arch/<arch>/arch_io.h` (実装側) だけで、**契約側の `include/io.h` も検査対象**
— 契約に実装が混ざったらそこで止まる。

命令列の一部としてしか意味を持たず切り出せない箇所は、その `asm` の直前の
コメントに `ARCH-ASM-OK` と**切り出せない理由**を書く。

## 今あるもの

    arch/x86/arch_io.h              i386 (唯一の実装)
    platform/pc98/platform_io.h     PC-9801/9821 (唯一の実装)

`arch/arm/` は**まだ無い**。この骨格は受け皿を先に用意するためのもので、
ARM 実装は別票で行う。
