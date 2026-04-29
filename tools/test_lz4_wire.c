/* LZ4デコーダ ホスト側ワイヤ互換テスト */
/* ビルド: gcc -O2 -w -o /tmp/test_lz4 tools/test_lz4_wire.c  */
/* 実行:   /tmp/test_lz4 /tmp/kernel.lz4 kernel.bin            */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;

#define LZ4_MINMATCH 4

static int lz4_decode(const u8 *src, int compressed_size,
                      u8 *dst, int dst_capacity)
{
    const u8 *ip = src, *ip_end = src + compressed_size;
    u8 *op = dst, *op_end = dst + dst_capacity;
    for (;;) {
        int token, lit_len, match_len, offset, i;
        const u8 *ms;
        if (ip >= ip_end) break;
        token = *ip++;
        lit_len = (token >> 4) & 0x0F;
        if (lit_len == 15) { int s; do { s = *ip++; lit_len += s; } while (s == 255); }
        if (ip + lit_len > ip_end || op + lit_len > op_end) return -1;
        for (i = 0; i < lit_len; i++) *op++ = *ip++;
        if (ip >= ip_end) break;
        offset = (int)ip[0] | ((int)ip[1] << 8); ip += 2;
        if (offset == 0 || op - dst < offset) return -1;
        match_len = (token & 0x0F) + LZ4_MINMATCH;
        if ((token & 0x0F) == 15) { int s; do { s = *ip++; match_len += s; } while (s == 255); }
        if (op + match_len > op_end) return -2;
        ms = op - offset;
        for (i = 0; i < match_len; i++) *op++ = ms[i];
    }
    return (int)(op - dst);
}

int main(int argc, char **argv)
{
    FILE *f; long csz, ofs; u8 *cb, *db, *ob; unsigned int os; int ds;
    if (argc != 3) { fprintf(stderr, "Usage: %s COMP ORIG\n", argv[0]); return 1; }
    f = fopen(argv[1], "rb"); fseek(f,0,2); csz=ftell(f); fseek(f,0,0);
    cb = malloc(csz); fread(cb,1,csz,f); fclose(f);
    os = cb[0]|(cb[1]<<8)|(cb[2]<<16)|(cb[3]<<24);
    db = malloc(os);
    ds = lz4_decode(cb+4, (int)(csz-4), db, (int)os); free(cb);
    if (ds < 0) { fprintf(stderr, "FAIL: decode err %d\n", ds); return 1; }
    f = fopen(argv[2], "rb"); fseek(f,0,2); ofs=ftell(f); fseek(f,0,0);
    ob = malloc(ofs); fread(ob,1,ofs,f); fclose(f);
    if (ds != (int)ofs || memcmp(db,ob,ds)) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("OK: Wire-compatible (%d bytes)\n", ds);
    free(db); free(ob); return 0;
}
