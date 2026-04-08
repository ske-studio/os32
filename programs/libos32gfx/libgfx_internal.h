#ifndef __LIBGFX_INTERNAL_H
#define __LIBGFX_INTERNAL_H

#include "libos32gfx.h"

static inline void _memcpy_w(void *dst, const void *src, unsigned int words) {
    __asm__ volatile("rep movsw" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
}

static inline void _memcpy_d(void *dst, const void *src, unsigned int dwords) {
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(dwords) : : "memory");
}

static inline void _memset_w(void *dst, unsigned int val16, unsigned int words) {
    __asm__ volatile("rep stosw" : "+D"(dst), "+c"(words) : "a"((unsigned short)val16) : "memory");
}

static inline void _memset_d(void *dst, unsigned int val32, unsigned int dwords) {
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(dwords) : "a"(val32) : "memory");
}

#define SURF_POOL_MAX  16
#define SURF_DATA_SIZE 2048

typedef struct {
    GFX_Surface surf;
    u8 data[4][SURF_DATA_SIZE];
    u8 used;
} SurfSlot;

#define SPR_SIZE_16  48
#define SPR_SIZE_32  160
#define SPR_SIZE_64  576
#define SPR_SIZE_128 2176

#define SPR_MAX_16   64
#define SPR_MAX_32   32
#define SPR_MAX_64   16
#define SPR_MAX_128  4

#define SPR_POOL_MAX (SPR_MAX_16 + SPR_MAX_32 + SPR_MAX_64 + SPR_MAX_128)

#define SPR_TYPE_NONE 0
#define SPR_TYPE_16   1
#define SPR_TYPE_32   2
#define SPR_TYPE_64   3
#define SPR_TYPE_128  4

typedef struct {
    GFX_Sprite spr;
    u8 used;
    u8 pool_type;
    int pool_idx;
    u8 *data_base;
} SprSlot;

typedef struct {
    u8 data_16[SPR_MAX_16][8][4][SPR_SIZE_16];
    u8 mask_16[SPR_MAX_16][8][SPR_SIZE_16];
    u8 bg_16[SPR_MAX_16][4][SPR_SIZE_16];
    u8 used_16[SPR_MAX_16];

    u8 data_32[SPR_MAX_32][8][4][SPR_SIZE_32];
    u8 mask_32[SPR_MAX_32][8][SPR_SIZE_32];
    u8 bg_32[SPR_MAX_32][4][SPR_SIZE_32];
    u8 used_32[SPR_MAX_32];

    u8 data_64[SPR_MAX_64][8][4][SPR_SIZE_64];
    u8 mask_64[SPR_MAX_64][8][SPR_SIZE_64];
    u8 bg_64[SPR_MAX_64][4][SPR_SIZE_64];
    u8 used_64[SPR_MAX_64];

    u8 data_128[SPR_MAX_128][8][4][SPR_SIZE_128];
    u8 mask_128[SPR_MAX_128][8][SPR_SIZE_128];
    u8 bg_128[SPR_MAX_128][4][SPR_SIZE_128];
    u8 used_128[SPR_MAX_128];
} SprDataPool;

#endif
