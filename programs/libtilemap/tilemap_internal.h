#ifndef __TILEMAP_INTERNAL_H
#define __TILEMAP_INTERNAL_H

#include "libtilemap.h"

typedef struct {
    KernelAPI *kapi;
    TileDef *tiles;       /* [MAX_TILES] */
    BGPlane *bg_planes;   /* [BG_COUNT] */
    int origin_x;
    int origin_y;
} TilemapState;

extern TilemapState _tilemap;

#endif /* __TILEMAP_INTERNAL_H */
