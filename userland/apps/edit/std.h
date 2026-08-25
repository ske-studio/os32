/*
 * std.h - OS32 VZ Editor
 * Translated from STD.INC (C89 compatible)
 */
#ifndef _VZ_STD_H_
#define _VZ_STD_H_

#include "types.h"
#include "os32api.h"

/* Typedefs replacing DOS/Assembly types */
typedef u8  BYTE;
typedef u16 WORD;
typedef u32 DWORD;

typedef i8  CHAR;
typedef i16 INT;
typedef i32 LONG;

/* Define standard pointers for flat memory model */
typedef BYTE* PBYTE;
typedef WORD* PWORD;
typedef void* PVOID;

/* Common Control Codes */
#define BELL    0x07
#define BS      0x08
#define TAB     0x09
#define LF      0x0A
#define CR      0x0D
#define EOF_CH  0x1A
#define ESCP    0x1B
#define SPC     0x20

/* Booleans */
#ifndef TRUE
#define TRUE    1
#define FALSE   0
#endif

#define ON      1
#define OFF     0

/* In OS32 NULL is normally defined in types.h, but we ensure it here */
#ifndef NULL
#define NULL    ((void *)0)
#endif
#define INVALID (-1)

/*
 * Note: MS-DOS specific constants (INT 21h functions, EMS, file attributes)
 * are largely omitted, as they will be replaced by OS32 KernelAPI wrappers.
 */

#endif /* _VZ_STD_H_ */
