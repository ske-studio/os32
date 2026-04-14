/* ======================================================================== */
/*  TEE.C -- 分岐出力                                                        */
/*                                                                          */
/*  Usage: tee FILE                                                          */
/*  stdin を stdout と FILE の両方に書き出す                                  */
/* ======================================================================== */
#include "os32api.h"
#include <string.h>
#include <stdio.h>

static KernelAPI *api;

int main(int argc, char **argv, KernelAPI *kapi)
{
    static char buf[65536];
    int sz, fd;
    api = kapi;

    if (argc < 2) {
        printf("Usage: tee FILE\n");
        return 1;
    }

    /* stdin から読み取り */
    sz = api->sys_read(0, buf, sizeof(buf));
    if (sz <= 0) return 0;

    /* stdout に書き出し */
    api->sys_write(1, buf, sz);

    /* ファイルにも書き出し */
    fd = api->sys_open(argv[1], KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd >= 0) {
        api->sys_write(fd, buf, sz);
        api->sys_close(fd);
    } else {
        fprintf(stderr, "tee: cannot open %s\n", argv[1]);
        return 1;
    }

    return 0;
}
