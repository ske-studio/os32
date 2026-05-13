/* ======================================================================== */
/*  V86_DISASM.C - V86 最小逆アセンブラ (T3.1)                              */
/*                                                                          */
/*  GP TRACE エントリの opcode バイト列を可読命令文字列に変換する。          */
/*  完全な x86 デコーダではなく、V86 でトレース必要な命令種別のみ対応:      */
/*    INT/IRET, CALL near/FAR, RET/RETF, IN/OUT/INSB/OUTSB,                */
/*    CLI/STI/HLT, JMP/Jcc, MOV r,imm, PUSH/POP, PUSHF/POPF               */
/* ======================================================================== */

#include "v86_disasm.h"

/* 16進数文字変換テーブル */
static const char hex_tbl[] = "0123456789ABCDEF";

/* 内部ヘルパー: バッファに文字列追加 */
static int da_pos;
static char *da_buf;
static int da_bufsz;

static void da_reset(char *buf, int bufsz)
{
    da_buf = buf;
    da_bufsz = bufsz;
    da_pos = 0;
    if (buf && bufsz > 0) buf[0] = '\0';
}

static void da_ch(char c)
{
    if (da_buf && da_pos < da_bufsz - 1) {
        da_buf[da_pos++] = c;
        da_buf[da_pos] = '\0';
    }
}

static void da_str(const char *s)
{
    while (*s) da_ch(*s++);
}

static void da_hex8(u8 v)
{
    da_ch(hex_tbl[v >> 4]);
    da_ch(hex_tbl[v & 0xF]);
}

static void da_hex16(u16 v)
{
    da_hex8((u8)(v >> 8));
    da_hex8((u8)v);
}

/* 16bit レジスタ名テーブル */
static const char *reg16_names[] = {
    "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"
};

/* 8bit レジスタ名テーブル */
static const char *reg8_names[] = {
    "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH"
};

/* 条件ジャンプニーモニック (0x70-0x7F) */
static const char *jcc_names[] = {
    "JO",  "JNO", "JB",  "JNB", "JZ",  "JNZ", "JBE", "JA",
    "JS",  "JNS", "JPE", "JPO", "JL",  "JGE", "JLE", "JG"
};

/* ====================================================================== */
/*  v86_disasm_one — 1命令を逆アセンブル                                   */
/* ====================================================================== */
int v86_disasm_one(const u8 *bytes, char *out, int outsz)
{
    const u8 *p = bytes;
    int prefix_66 = 0;
    u8 op;

    da_reset(out, outsz);

    /* プレフィックスループ */
    for (;;) {
        if (*p == 0x66) { prefix_66 = 1; p++; continue; }
        if (*p == 0x67) { p++; continue; }
        if (*p == 0x26 || *p == 0x2E || *p == 0x36 || *p == 0x3E ||
            *p == 0x64 || *p == 0x65) { p++; continue; }
        if (*p == 0xF0) { p++; continue; }
        if (*p == 0xF3) { da_str("REP "); p++; continue; }
        if (*p == 0xF2) { da_str("REPNE "); p++; continue; }
        break;
    }

    op = *p++;

    switch (op) {

    /* ================================================================ */
    /*  INT n / INT 3 / INTO                                            */
    /* ================================================================ */
    case 0xCC:
        da_str("INT 3");
        break;
    case 0xCD:
        da_str("INT ");
        da_hex8(*p++);
        da_ch('h');
        break;
    case 0xCE:
        da_str("INTO");
        break;

    /* ================================================================ */
    /*  IRET / IRETD                                                    */
    /* ================================================================ */
    case 0xCF:
        da_str(prefix_66 ? "IRETD" : "IRET");
        break;

    /* ================================================================ */
    /*  CLI / STI / HLT / NOP / CLC / STC / CLD / STD                  */
    /* ================================================================ */
    case 0xFA: da_str("CLI"); break;
    case 0xFB: da_str("STI"); break;
    case 0xF4: da_str("HLT"); break;
    case 0x90: da_str("NOP"); break;
    case 0xF8: da_str("CLC"); break;
    case 0xF9: da_str("STC"); break;
    case 0xFC: da_str("CLD"); break;
    case 0xFD: da_str("STD"); break;

    /* ================================================================ */
    /*  PUSHF / POPF / PUSHFD / POPFD                                   */
    /* ================================================================ */
    case 0x9C: da_str(prefix_66 ? "PUSHFD" : "PUSHF"); break;
    case 0x9D: da_str(prefix_66 ? "POPFD" : "POPF"); break;

    /* ================================================================ */
    /*  RET near / RETF                                                 */
    /* ================================================================ */
    case 0xC3: da_str("RET"); break;
    case 0xC2:
        da_str("RET ");
        da_hex16(*(u16 *)p);
        p += 2;
        break;
    case 0xCB: da_str("RETF"); break;
    case 0xCA:
        da_str("RETF ");
        da_hex16(*(u16 *)p);
        p += 2;
        break;

    /* ================================================================ */
    /*  CALL near rel16                                                 */
    /* ================================================================ */
    case 0xE8: {
        i16 rel = *(i16 *)p;
        p += 2;
        da_str("CALL ");
        da_hex16((u16)((int)(p - bytes) + (int)rel));
        break;
    }

    /* ================================================================ */
    /*  CALL FAR ptr16:16 (直接)                                       */
    /* ================================================================ */
    case 0x9A: {
        u16 off = *(u16 *)p; p += 2;
        u16 seg = *(u16 *)p; p += 2;
        da_str("CALL FAR ");
        da_hex16(seg);
        da_ch(':');
        da_hex16(off);
        break;
    }

    /* ================================================================ */
    /*  JMP near rel16 / JMP short rel8                                 */
    /* ================================================================ */
    case 0xE9: {
        i16 rel = *(i16 *)p;
        p += 2;
        da_str("JMP ");
        da_hex16((u16)((int)(p - bytes) + (int)rel));
        break;
    }
    case 0xEB: {
        i8 rel = *(i8 *)p;
        p++;
        da_str("JMP SHORT ");
        da_hex16((u16)((int)(p - bytes) + (int)rel));
        break;
    }

    /* ================================================================ */
    /*  JMP FAR ptr16:16                                                */
    /* ================================================================ */
    case 0xEA: {
        u16 off = *(u16 *)p; p += 2;
        u16 seg = *(u16 *)p; p += 2;
        da_str("JMP FAR ");
        da_hex16(seg);
        da_ch(':');
        da_hex16(off);
        break;
    }

    /* ================================================================ */
    /*  Jcc rel8 (0x70-0x7F)                                            */
    /* ================================================================ */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        i8 rel = *(i8 *)p;
        p++;
        da_str(jcc_names[op - 0x70]);
        da_ch(' ');
        da_hex16((u16)((int)(p - bytes) + (int)rel));
        break;
    }

    /* ================================================================ */
    /*  LOOP / LOOPZ / LOOPNZ / JCXZ                                   */
    /* ================================================================ */
    case 0xE0: { i8 r = *(i8 *)p; p++; da_str("LOOPNZ "); da_hex16((u16)((int)(p-bytes)+(int)r)); break; }
    case 0xE1: { i8 r = *(i8 *)p; p++; da_str("LOOPZ ");  da_hex16((u16)((int)(p-bytes)+(int)r)); break; }
    case 0xE2: { i8 r = *(i8 *)p; p++; da_str("LOOP ");   da_hex16((u16)((int)(p-bytes)+(int)r)); break; }
    case 0xE3: { i8 r = *(i8 *)p; p++; da_str("JCXZ ");   da_hex16((u16)((int)(p-bytes)+(int)r)); break; }

    /* ================================================================ */
    /*  IN / OUT (即値ポート / DXポート)                                */
    /* ================================================================ */
    case 0xE4:
        da_str("IN AL, ");
        da_hex8(*p++);
        da_ch('h');
        break;
    case 0xE5:
        da_str(prefix_66 ? "IN EAX, " : "IN AX, ");
        da_hex8(*p++);
        da_ch('h');
        break;
    case 0xE6:
        da_str("OUT ");
        da_hex8(*p++);
        da_str("h, AL");
        break;
    case 0xE7:
        da_str("OUT ");
        da_hex8(*p++);
        da_str(prefix_66 ? "h, EAX" : "h, AX");
        break;
    case 0xEC: da_str("IN AL, DX"); break;
    case 0xED: da_str(prefix_66 ? "IN EAX, DX" : "IN AX, DX"); break;
    case 0xEE: da_str("OUT DX, AL"); break;
    case 0xEF: da_str(prefix_66 ? "OUT DX, EAX" : "OUT DX, AX"); break;

    /* ================================================================ */
    /*  INSB / INSW / OUTSB / OUTSW                                    */
    /* ================================================================ */
    case 0x6C: da_str("INSB"); break;
    case 0x6D: da_str(prefix_66 ? "INSD" : "INSW"); break;
    case 0x6E: da_str("OUTSB"); break;
    case 0x6F: da_str(prefix_66 ? "OUTSD" : "OUTSW"); break;

    /* ================================================================ */
    /*  PUSH r16 (0x50-0x57) / POP r16 (0x58-0x5F)                     */
    /* ================================================================ */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        da_str("PUSH ");
        da_str(reg16_names[op - 0x50]);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        da_str("POP ");
        da_str(reg16_names[op - 0x58]);
        break;

    /* ================================================================ */
    /*  MOV r8, imm8 (0xB0-0xB7)                                       */
    /* ================================================================ */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        da_str("MOV ");
        da_str(reg8_names[op - 0xB0]);
        da_str(", ");
        da_hex8(*p++);
        da_ch('h');
        break;

    /* ================================================================ */
    /*  MOV r16, imm16 (0xB8-0xBF)                                     */
    /* ================================================================ */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        da_str("MOV ");
        da_str(reg16_names[op - 0xB8]);
        da_str(", ");
        if (prefix_66) {
            u16 hi, lo;
            lo = *(u16 *)p; p += 2;
            hi = *(u16 *)p; p += 2;
            da_hex16(hi);
            da_hex16(lo);
        } else {
            da_hex16(*(u16 *)p);
            p += 2;
        }
        da_ch('h');
        break;

    /* ================================================================ */
    /*  0xFF グループ — CALL FAR indirect / JMP indirect / PUSH r/m     */
    /* ================================================================ */
    case 0xFF: {
        u8 modrm = *p++;
        u8 reg = (modrm >> 3) & 0x07;

        switch (reg) {
        case 2:  da_str("CALL [modrm]"); break;
        case 3:  da_str("CALL FAR [modrm]"); break;
        case 4:  da_str("JMP [modrm]"); break;
        case 5:  da_str("JMP FAR [modrm]"); break;
        case 6:  da_str("PUSH [modrm]"); break;
        default: da_str("FF/?"); da_hex8(reg); break;
        }
        /* modrm のディスプレースメントを消費 (簡略版) */
        {
            u8 mod = modrm >> 6;
            u8 rm  = modrm & 0x07;
            if (mod == 1) { p++; }          /* disp8 */
            else if (mod == 2) { p += 2; }  /* disp16 */
            else if (mod == 0 && rm == 6) { p += 2; } /* [disp16] */
        }
        break;
    }

    /* ================================================================ */
    /*  XCHG AX, r16 (0x91-0x97)                                       */
    /* ================================================================ */
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97:
        da_str("XCHG AX, ");
        da_str(reg16_names[op - 0x90]);
        break;

    /* ================================================================ */
    /*  CBW / CWD                                                       */
    /* ================================================================ */
    case 0x98: da_str(prefix_66 ? "CWDE" : "CBW"); break;
    case 0x99: da_str(prefix_66 ? "CDQ" : "CWD"); break;

    /* ================================================================ */
    /*  不明オペコード                                                  */
    /* ================================================================ */
    default:
        da_str("db ");
        da_hex8(op);
        da_ch('h');
        break;
    }

    return (int)(p - bytes);
}
