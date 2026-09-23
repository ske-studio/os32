/* ======================================================================== */
/*  KBD_STATUS_HOST.C — drivers/kbd_status.c をそのままホストで回す         */
/*                                                                          */
/*  実物を 1 行も写さずに #include する。I/O を持たないので模型は要らない。 */
/*  見るのは IRQ1 ハンドラが 0041h を読むか・使うかの 3 分岐                 */
/*  (記録: kbd_status_tdd.md、経緯: docs/POLICY_DEBUG.md §4-57):            */
/*    (a) RxRDY = 0 → EMPTY (エラービットが立っていても 0041h は読まない)    */
/*    (b) RxRDY = 1 で PE / OE / FE のどれか 1 つでも → ERROR                */
/*    (c) RxRDY = 1 でエラー無し → DATA。他のビット (TxRDY / TxEMP / BRK /  */
/*        DSR) は判定に効かない — NP21/W の keyboard_i43 は `status | 0x85`  */
/*        (DSR / TxEMP / TxRDY を常に立てる) を返すので、そこで DATA に      */
/*        ならないとエミュレータの打鍵が全部落ちる。                         */
/* ======================================================================== */
#include <stdio.h>
#include <string.h>
#include "../../drivers/kbd_status.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); failed++; \
} } while (0)

static int failed;

/* (a) 空 IRQ */
static void case_empty(void)
{
    CHECK(kbd_status_classify(0x00) == KBD_ST_EMPTY);
    CHECK(kbd_status_classify(0x85) == KBD_ST_EMPTY);   /* NP21/W の空 */
    CHECK(kbd_status_classify(0x38) == KBD_ST_EMPTY);   /* エラーだけ */
    CHECK(kbd_status_classify(0xFD) == KBD_ST_EMPTY);   /* bit1 以外全部 */
}

/* (b) エラー。1 ビットずつ */
static void case_error(void)
{
    CHECK(kbd_status_classify(0x02 | 0x08) == KBD_ST_ERROR);   /* PE */
    CHECK(kbd_status_classify(0x02 | 0x10) == KBD_ST_ERROR);   /* OE */
    CHECK(kbd_status_classify(0x02 | 0x20) == KBD_ST_ERROR);   /* FE */
    CHECK(kbd_status_classify(0x85 | 0x02 | 0x10) == KBD_ST_ERROR); /* NP21/W の溢れ */
    CHECK(kbd_status_classify(0xFF) == KBD_ST_ERROR);
}

/* (c) 正常。エラー以外のビットは効かない */
static void case_data(void)
{
    unsigned int other;
    CHECK(kbd_status_classify(0x02) == KBD_ST_DATA);
    CHECK(kbd_status_classify(0x85 | 0x02) == KBD_ST_DATA);   /* NP21/W の打鍵 */
    /* bit0 / bit2 / bit6 / bit7 の全組み合わせ */
    for (other = 0; other < 16; other++) {
        unsigned char st = (unsigned char)(0x02
            | ((other & 1) ? 0x01 : 0) | ((other & 2) ? 0x04 : 0)
            | ((other & 4) ? 0x40 : 0) | ((other & 8) ? 0x80 : 0));
        CHECK(kbd_status_classify(st) == KBD_ST_DATA);
    }
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "empty")) case_empty();
    else if (!strcmp(c, "error")) case_error();
    else if (!strcmp(c, "data")) case_data();
    else { fprintf(stderr, "unknown case %s\n", c); return 2; }
    return failed ? 1 : 0;
}
