#ifndef __GFX_INTERNAL_H
#define __GFX_INTERNAL_H

#include "gfx.h"
#include "pc98.h"
#include "memmap.h"
#include "palette.h"

/* ======================================================================== */
/*  内部変数アクセス                                                        */
/* ======================================================================== */
extern u8 *bb[4];
extern u8 *bb_b;
extern u8 *bb_r;
extern u8 *bb_g;
extern u8 *bb_i;

extern int dirty_min_y;
extern int dirty_max_y;

extern int vram_scroll_y;

void gfx_dirty_mark(int y0, int y1);

void gfx_surface_init(void);
void gfx_sprite_init(void);
void gfx_scroll_init(void);

/* ======================================================================== */
/*  I/O・メモリ操作インライン                                               */
/* ======================================================================== */
#define GDC_CMD_PORT    0xA2
#define GDC_PRM_PORT    0xA0
#define GDC_CMD_SCROLL  0x70

static inline void _out(unsigned int port, unsigned int val) {
    __asm__ volatile("outb %b0, %w1" : : "a"((unsigned char)val), "Nd"(port));
}

static inline unsigned int _in(unsigned int port) {
    unsigned char ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void _memcpy_w(void *dst, const void *src, unsigned int words) {
    __asm__ volatile("rep movsw"
                     : "+D"(dst), "+S"(src), "+c"(words)
                     : : "memory");
}

static inline void _memcpy_d(void *dst, const void *src, unsigned int dwords) {
    __asm__ volatile("rep movsl"
                     : "+D"(dst), "+S"(src), "+c"(dwords)
                     : : "memory");
}

static inline void _memset_w(void *dst, unsigned int val16, unsigned int words) {
    __asm__ volatile("rep stosw"
                     : "+D"(dst), "+c"(words)
                     : "a"((unsigned short)val16)
                     : "memory");
}

static inline void _memset_d(void *dst, unsigned int val32, unsigned int dwords) {
    __asm__ volatile("rep stosl"
                     : "+D"(dst), "+c"(dwords)
                     : "a"(val32)
                     : "memory");
}

/* ======================================================================== */
/*  プール構造体                                                            */
/* ======================================================================== */
#define SURF_POOL_MAX  16
#define SURF_DATA_SIZE 2048

typedef struct {
    GFX_Surface surf;
    u8 data[4][SURF_DATA_SIZE];
    u8 used;
} SurfSlot;

#define SPR_POOL_MAX  16
#define SPR_DATA_SIZE 2048

typedef struct {
    GFX_Sprite spr;
    u8 data[4][SPR_DATA_SIZE];
    u8 mask_data[SPR_DATA_SIZE];
    u8 used;
} SprSlot;

#define SPR_POOL_ADDR   (MEM_GFX_SURF_POOL + sizeof(SurfSlot) * SURF_POOL_MAX)

#endif /* __GFX_INTERNAL_H */
