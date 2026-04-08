/* ======================================================================== */
/*  EXEC.H — 簡易プログラムローダー                                         */
/*                                                                          */
/*  ext2上のフラットバイナリを拡張メモリにロードして実行する。               */
/*  外部プログラムはKernelAPI構造体を通じてカーネル関数を呼び出す。          */
/*                                                                          */
/*  呼び出し規約:                                                           */
/*    カーネル: System V i386 ABI                                            */
/*    外部プログラム: System V i386 ABI                                      */
/*    KernelAPIの関数ポインタ: __cdecl ラッパー経由                          */
/*    ExecEntry (外部プログラムのmain): __cdecl                             */
/*                                                                          */
/*  メモリ配置:                                                             */
/*    0x3F0000 : KernelAPI テーブル (関数ポインタ群)                        */
/*    0x400000 : プログラムロード領域 (最大1MB)                             */
/* ======================================================================== */

#ifndef __EXEC_H
#define __EXEC_H

/* KernelAPI構造体・OS32Header・基本型は共有ヘッダから取得 */
#include "os32_kapi_shared.h"
#include "memmap.h"

/* プログラムのロード先 (固定) */
#define EXEC_LOAD_ADDR    MEM_EXEC_LOAD_ADDR
#define EXEC_MAX_SIZE     MEM_EXEC_MAX_SIZE
/* ======== API ======== */
void exec_init(void);
int exec_run(const char *cmdline);

#endif /* __EXEC_H */
