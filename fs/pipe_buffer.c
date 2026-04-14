/* ======================================================================== */
/*  PIPE_BUFFER.C -- パイプバッファ管理                                       */
/*                                                                          */
/*  シングルタスクOS向けの静的パイプバッファ。                                */
/*  kmalloc の代わりに静的配列を使用 (カーネルBSS内に配置)。                  */
/* ======================================================================== */

#include "pipe_buffer.h"

/* 静的パイプバッファ (BSS領域、kentry.asmでゼロクリア済み) */
static u8 pipe_data[PIPE_BUF_COUNT][PIPE_BUF_SIZE];
static u32 pipe_len[PIPE_BUF_COUNT];
static int pipe_used[PIPE_BUF_COUNT];

void pipe_buffer_init(void)
{
    int i;
    for (i = 0; i < PIPE_BUF_COUNT; i++) {
        pipe_used[i] = 0;
        pipe_len[i] = 0;
    }
}

int pipe_alloc(void)
{
    int i;
    for (i = 0; i < PIPE_BUF_COUNT; i++) {
        if (!pipe_used[i]) {
            pipe_used[i] = 1;
            pipe_len[i] = 0;
            return i;
        }
    }
    return -1;
}

void pipe_free(int id)
{
    if (id >= 0 && id < PIPE_BUF_COUNT) {
        pipe_used[id] = 0;
        pipe_len[id] = 0;
    }
}

u8 *pipe_get_buf(int id)
{
    if (id >= 0 && id < PIPE_BUF_COUNT) {
        return pipe_data[id];
    }
    return (u8 *)0;
}

u32 pipe_get_capacity(int id)
{
    if (id >= 0 && id < PIPE_BUF_COUNT) {
        return PIPE_BUF_SIZE;
    }
    return 0;
}

u32 pipe_get_len(int id)
{
    if (id >= 0 && id < PIPE_BUF_COUNT) {
        return pipe_len[id];
    }
    return 0;
}

void pipe_set_len(int id, u32 len)
{
    if (id >= 0 && id < PIPE_BUF_COUNT) {
        pipe_len[id] = (len > PIPE_BUF_SIZE) ? PIPE_BUF_SIZE : len;
    }
}

void pipe_clear(int id)
{
    if (id >= 0 && id < PIPE_BUF_COUNT) {
        pipe_len[id] = 0;
    }
}
