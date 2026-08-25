/*
 * vz.h - OS32 VZ Editor
 * Translated from VZ.INC (C89 compatible)
 */
#ifndef _VZ_H_
#define _VZ_H_

#include "std.h"
#include "../../lib/utf8.h"
#include "lconsole.h"
#include <stdlib.h>

/* --- System Modes & Constants --- */
#define TE_INITIAL_BUFFER_SIZE 65536  /* ギャップバッファ初期/拡張単位 (64KB) */

#define SYS_SEDIT 0
#define SYS_GETS  1
#define SYS_DOS   2
#define SYS_FILER 3
#define SYS_GETC  4

#define PATHSZ 64
#define MASKSZ 32
#define MARKCNT 4

#define EDIT_VERSION "1.6"
#define EDIT_BANNER  "OS32 Edit - VZ Editor inspired, AI-assisted port"

/* --- Data Structures --- */

/* Directoy record */
typedef struct _DIR_REC {
    BYTE dr_attr;
    WORD dr_time;
    WORD dr_date;
    DWORD dr_size;
    BYTE dr_pack[13];
} DIR_REC;

/* Filer window record */
#define MAX_FILER_ENTRIES 128
typedef struct {
    char name[16];
    DWORD size;
    BYTE is_dir;
} FilerEntry;

typedef struct _FILER {
    /* OS32 native Filer extensions */
    int active;
    FilerEntry entries[MAX_FILER_ENTRIES];
    int entry_count;
    int cursor_idx;
    int scroll_top;
    char current_dir[PATHSZ];
    
    /* VZ original fields */
    void* fl_back;
    void* fl_pooltop;
    void* fl_poolend;
    void* fl_poolp;
    WORD fl_selcnt;
    void* fl_bcsr;
    WORD fl_files;
    void* fl_home;
    BYTE fl_wpx, fl_wpy, fl_wsx, fl_wsy;
    WORD fl_tsy;
    BYTE fl_ttlsx, fl_which;
    DWORD fl_free;
    WORD fl_clust;
    BYTE fl_overflow, fl_curf;
    char fl_path[PATHSZ];
    char fl_mask[MASKSZ];
    char fl_lastpath[PATHSZ];
} FILER;

/* Text record (previously _text struc) */
typedef struct _TEXT {
    struct _TEXT* w_next;   /* next record link ptr */
    BYTE wnum;              /* window number */
    CHAR tchf;              /* touch flag (0=nop,1=modified,-1=RO) */
    BYTE wsplit;            /* window split mode */
    BYTE blkm;              /* block mode (1=line,2=char) */
    BYTE wy;                /* y loc. in screen */
    BYTE wnxt;              /* next line y loc. in screen */
    BYTE wys;               /* wy keeper */

    BYTE nodnumb;           /* disp number flag */
    WORD lnumb;             /* line number */
    WORD dnumb;             /* disp number */
    WORD lnumb0;            /* line number offset */
    WORD dnumb0;            /* disp number offset */

    /* Pointer to text buffer (flat model, replacing EMS/segment logic) */
    void* ttop;             /* text top ptr */
    void* tend;             /* text end ptr */
    void* tmax;             /* text buffer end ptr */
    void* thom;             /* screen home ptr */
    void* tbtm;             /* screen bottom ptr */
    void* tnow;             /* current line ptr */
    void* tnxt;             /* next line ptr */

    void* btop;             /* line buffer top ptr */
    void* bend;             /* line end ptr */
    void* tcp;              /* current ptr (common) */
    void* tfld;             /* field start ptr (common) */
    void* bmax;             /* line buffer end ptr */
    void* bhom;             /* screen home ptr (in buffer) */
    void* bbtm;             /* screen bottom ptr (in buffer) */

    BYTE inbuf;             
    BYTE tw_px, tw_py, tw_sx, tw_sy, tw_cy;
    BYTE fsiz;              /* field size */
    BYTE fskp;              /* display skip x */
    BYTE fofs;              /* H-scroll offset x */
    BYTE lxs;               /* lx keeper */
    BYTE lx;                /* x loc. in field */
    BYTE ly;                /* y loc. in line */
    BYTE tabr;              /* Tab size */
    BYTE exttyp;            /* file ext type */
    BYTE ctype;             /* current char type */
    BYTE ckanj;             /* 1=kanji */
    WORD ccode;             /* current char code */
    
    char* namep;            /* file name ptr */

    BYTE largf;             /* large text flag */
    BYTE temp;              /* temporary file flag */

    DWORD readp;            /* text read offset */
    DWORD eofp;             /* EOF offset */
    DWORD headp;            /* head offset */
    WORD headsz;            /* head size */
    DWORD tailp;            /* tail offset */
    WORD tailsz;            /* tail size */
    WORD textid;            /* text ID No. */
    
    WORD w1, w2, w3;        /* works */
    WORD bofs;              /* block mark offset in line */
    BYTE blkx;              /* block mark x loc. */
    BYTE fsiz0;             /* save of fsiz */

    DWORD tbmax;            /* size of text buffer */
    DWORD tbalt;            /* size of temp. block */

    DWORD tblkp;            /* block lptr */
    DWORD tnowp;            /* current lptr */
    DWORD trgtp;            /* target lptr */
    DWORD toldp;            /* old current lptr */
    DWORD tretp;            /* return lptr */
    DWORD tmark[MARKCNT];   /* mark lptr */

    char path[PATHSZ];      /* path name area */
    char* tsstr;            /* title search string ptr */
    WORD blktgt;            /* block target flag */
    WORD inpcnt;            /* input counter */
    void* ektbl;            /* event key table ptr */
    WORD extword;           /* file ext word */
    char* labelp;           /* label name ptr */
    WORD lnumb9;            /* last line number */
    
    BYTE dspsw1;            /* alternate dspsw */
    BYTE atrtxt1;
    BYTE atrstt1;
    BYTE atrpath1;

    /* --- Block States --- */
    int tblock_start;       /* Logical index (0 to size), -1=none */
    int tblock_end;         /* Logical index (0 to size), -1=none */
    int block_mode;         /* 0=なし, 1=選択中(B), 2=確定(K), 3=矩形(B再押下) */
    int block_start_col;    /* 矩形選択: 開始列 */
    int block_end_col;      /* 矩形選択: 終了列 */

    /* --- Line Number Caches --- */
    void* last_tcp;         /* 前回の te_count_position() 時の tcp */
    int   last_tcp_line;    /* そのときの tcp の行番号(lnumb) */
    void* last_thom;        /* 前回の su_draw_text() 時の thom */
    int   last_thom_line;   /* そのときの thom の行番号(disp_line) */
} TEXT;

/* --- Undo Structures --- */
#define UNDO_MAX 64
#define UNDO_DATA_MAX 128

#define UNDO_INSERT 1
#define UNDO_DELETE 2

typedef struct UndoEntry {
    int type;                  /* 1=挿入された, 2=削除された */
    int position;              /* 操作が発生した論理インデックス */
    char data[UNDO_DATA_MAX];  /* 対象文字列 */
    int data_len;              /* 文字列長 */
} UndoEntry;

/* --- Macro Engine Structures --- */
#define MACRO_CALL_MAX 8
#define KEY_MAP_MAX 128

typedef struct {
    int key_code;
    int prefix; /* 0=None, 1=Ctrl-K, 2=Ctrl-Q */
    const char* macro;
} KeyBind;

typedef struct {
    const char* macro_text; /* 現在実行中のマクロ文字列 */
    int pc;                 /* プログラムカウンタ */
    const char* call_stack[MACRO_CALL_MAX];
    int call_pc_stack[MACRO_CALL_MAX];
    int call_depth;
    int running;            /* 実行中フラグ */
    int result_flag;        /* 条件分岐等に使う状態 */
} MacroEngine;

/* --- Command Dispatch Structures --- */
typedef void (*CommandFunc)(TEXT* w);

typedef struct {
    int prefix; /* 0=None, 1=Ctrl-K, 2=Ctrl-Q */
    int key;    /* c code */
    CommandFunc func;
} KeyMap;

/* --- Editor Global State --- */
#define MAX_TEXTS 4

typedef struct _VZGLOBAL {
    TEXT* current_text;     /* 現在アクティブなTEXTへのポインタ */
    
    /* --- Multi-Text & Multi-Window --- */
    TEXT* text_list[MAX_TEXTS];
    int text_count;
    
    int split_mode;         /* 0=単一ウインドウ, 1=上下分割 */
    int window_text_idx[2]; /* 上(0)と下(1)のウインドウに割り当てられたtext_listのインデックス */
    int active_window;      /* 0=上, 1=下 */

    FILER filer;
    /* Notifications */
    char notification[128];
    int notification_timer;
    
    /* Help Popup State */
    int help_active;
    int insert_mode;        /* 1=挿入モード, 0=上書きモード */
    char search_buf[64];    /* 検索文字列 */
    char replace_buf[64];   /* 置換文字列 */
    int search_dir;         /* 検索方向 (1=前方, -1=後方) */
    int search_ignore_case; /* 1=大文字小文字無視 */

    /* --- Clipboard --- */
    char* clipboard_buf;
    int clipboard_len;
    int clipboard_max;      /* mem_allocしたサイズ */

    /* --- Undo / Redo --- */
    UndoEntry undo_stack[UNDO_MAX];
    int undo_top;           /* 次に書き込むリングバッファインデックス */
    int undo_count;         /* 現在保持している履歴の数 (最大UNDO_MAX) */
    int undo_ptr;           /* Undo/Redoを前後するための仮想ポインタ */
    int undo_in_progress;   /* Undo実行中のフラグ（再帰記録を防ぐため）*/
    /* --- Macro Engine --- */
    MacroEngine macro_env;
    KeyBind key_map[KEY_MAP_MAX];
    int key_bind_count;
    
    /* Other global states will be added here incrementally */
} VZGLOBAL;

/* The single global state instance */
extern VZGLOBAL vz;
extern int vz_screen_width;
extern int vz_screen_height;
extern int vz_quit_flag;

/* KernelAPI pointer for standard library-like calls */
extern KernelAPI* kapi;

#define os32_printf(fmt, ...) kapi->kprintf(0xE1, fmt, ##__VA_ARGS__)
#define os32_putchar(c) kapi->shell_putchar(c, 0xE1)
#define os32_getch() kapi->kbd_trygetchar()
#define os32_malloc(s) malloc(s)
#define os32_free(p) free(p)

/* --- File I/O External Interfaces --- */
int vz_open(const char *path, int mode);
int vz_read(int fd, void *buf, unsigned int size);
int vz_write(int fd, const void *buf, unsigned int size);
int vz_seek(int fd, int offset, int whence);
void vz_close(int fd);
unsigned int vz_get_size(int fd);
void vz_load_file(const char* path); /* From main.c */
int vz_ls(const char *path, void *cb, void *ctx);

/* --- Screen VRAM (lconsole経由) --- */
void su_sync_vram(void);
void su_invalidate_vram(void);

/* --- OS32 Wrapper Prototypes (sys_*.c) --- */
void vz_clear(void);
void vz_set_cursor(int x, int y);
void vz_putc(int x, int y, char ch, unsigned char attr);
void vz_putkanji(int x, int y, unsigned short jis, unsigned char attr);

int vz_kbhit(void);
int vz_getch(void);
unsigned int vz_get_modifiers(void);

/* --- Shift-JIS Utilities --- */
#define IS_SJIS_1ST(c) (((unsigned char)(c) >= 0x81 && (unsigned char)(c) <= 0x9F) || \
                        ((unsigned char)(c) >= 0xE0 && (unsigned char)(c) <= 0xFC))
#define IS_SJIS_2ND(c) (((unsigned char)(c) >= 0x40 && (unsigned char)(c) <= 0x7E) || \
                        ((unsigned char)(c) >= 0x80 && (unsigned char)(c) <= 0xFC))

/* --- Text Edit Engine Prototypes --- */
void te_init_buffer(TEXT* w);
int te_resize_buffer(TEXT* w, unsigned int new_size);
void te_insert_char(TEXT* w, int c);
void te_delete_char(TEXT* w, int backspace);
void te_move_cursor(TEXT* w, int dx, int dy);
void te_count_position(TEXT* w);
void te_move_line(TEXT* w, int dy);
void te_move_to_bol(TEXT* w);
void te_move_to_eol(TEXT* w);
void te_move_to_top(TEXT* w);
void te_move_to_bottom(TEXT* w);
int te_save_file(TEXT* w); /* ファイル保存 */
int te_search_forward(TEXT* w, const char* query);
int te_search_backward(TEXT* w, const char* query);
int te_search_forward_ci(TEXT* w, const char* query);
int te_search_backward_ci(TEXT* w, const char* query);
int te_replace(TEXT* w, const char* query, const char* replacement, int all);

int te_get_logical_index(TEXT* w, char* p);
char* te_get_pointer_from_index(TEXT* w, int index);
void te_move_to_index(TEXT* w, int target_idx);

void te_calc_block_bounds(TEXT* w, int* p_bs, int* p_be, int* p_rect_mode, int* p_bs_line, int* p_be_line, int* p_bs_col, int* p_be_col);

void te_delete_line(TEXT* w);
void te_mark_block_start(TEXT* w);
void te_mark_block_end(TEXT* w);
void te_unmark_block(TEXT* w);
void te_block_copy(TEXT* w);
void te_block_delete(TEXT* w);
void te_block_paste(TEXT* w);

void te_record_undo_insert(char c, int pos);
void te_record_undo_delete(char c, int pos);
void te_undo(TEXT* w);
void te_redo(TEXT* w);

/* --- 行ジャンプ --- */
void te_goto_line(TEXT* w, int line_num);

/* --- 矩形ブロック操作 --- */
void te_block_copy_rect(TEXT* w);
void te_block_delete_rect(TEXT* w);
void te_block_paste_rect(TEXT* w);

/* --- バッファ縮小 --- */
int te_shrink_buffer(TEXT* w);

/* --- PC-98 テキスト属性 --- */
#define ATR_NORMAL   0xE1           /* 白文字, 通常表示 */
#define ATR_STATUS   0xE5           /* 白文字, 反転表示 (RV=1) */
#define ATR_FUNCBAR  0xE5           /* 白文字, 反転表示 */
#define ATR_LINENUM  0xA1           /* シアン(水色)文字, 通常表示 */
#define ATR_NEWLINE  0xA1           /* シアン, 改行マーク */
#define ATR_EOF      0xA1           /* シアン, EOF マーク */

/* 画面サイズ定数 */
#define SCREEN_W     vz_screen_width
#define SCREEN_H     vz_screen_height
#define FUNCBAR_Y    (vz_screen_height - 1)

/* --- Screen Utilities (screen_util.c) --- */
void su_fill_rect(int start_x, int start_y, int w, int h, char ch, unsigned char attr);
void su_clear_line(int y, unsigned char attr);
int su_int_to_str(int val, char *buf);
int su_put_int(int x, int y, int val, int width, int right_align, char pad_char, unsigned char attr);
int su_put_string(int x, int y, const char *s, unsigned char attr);
int su_put_utf8_char(int x, int y, utf8_decode_t *dec, unsigned char attr);
int su_put_utf8_string(int x, int y, const char *utf8_str, unsigned char attr);

/* --- Screen UI (screen_ui.c) --- */
void su_draw_statusbar(TEXT* w);
void su_draw_funcbar(void);
void su_prompt(const char* prompt, const char* buffer, int cursor_pos);
void su_draw_help(void);

/* --- Screen Text (screen_text.c) --- */
void su_draw_text(TEXT* w);
void su_adjust_scroll(TEXT* w);

/* --- Screen Update (screen_update.c & screen_filer.c) --- */
void su_redraw_screen(void);
void su_redraw_window(TEXT* w);
void su_redraw_filer(void);

/* --- Macro Engine Prototypes --- */
void macro_init(void);
void macro_start(const char* mac_str);
int macro_step(void);
void macro_exec_builtin(int cmd);
void set_notification(const char* msg); /* From main.c */
int vz_prompt_input(const char* prompt, char* buf, int buf_size); /* From main.c */
void vz_open_filer(const char* dir);
int vz_filer_handle_key(int c);

/* --- Command Handlers --- */
void cmd_save_and_quit(TEXT* t);
void cmd_save(TEXT* t);
void cmd_save_as(TEXT* t);
void cmd_open_file(TEXT* t);
void cmd_filer(TEXT* t);
void cmd_swap_text(TEXT* t);
void cmd_swap_window(TEXT* t);
void cmd_toggle_split(TEXT* t);
void cmd_block_start(TEXT* t);
void cmd_block_end(TEXT* t);
void cmd_block_hide(TEXT* t);
void cmd_block_copy(TEXT* t);
void cmd_block_delete(TEXT* t);
void cmd_redo(TEXT* t);
void cmd_help(TEXT* t);
void cmd_top(TEXT* t);
void cmd_bottom(TEXT* t);
void cmd_search_forward(TEXT* t);
void cmd_search_next(TEXT* t);
void cmd_search_backward(TEXT* t);
void cmd_goto_line(TEXT* t);
void cmd_replace(TEXT* t);
void cmd_replace_all(TEXT* t);
void cmd_toggle_case(TEXT* t);

/* --- PC-98 キーコード定義 (PC9800Bible 表2-13 準拠) --- */
/* kbd_trygetkey() / kbd_getkey() が返すキーコードデータの上位バイト */
#define VK_ESC      0x00
#define VK_BS       0x0E
#define VK_TAB      0x0F
#define VK_RETURN   0x1C
#define VK_SPACE    0x34
#define VK_XFER     0x35
#define VK_ROLLUP   0x36
#define VK_ROLLDOWN 0x37
#define VK_INS      0x38
#define VK_DEL      0x39
#define VK_UP       0x3A
#define VK_LEFT     0x3B
#define VK_RIGHT    0x3C
#define VK_DOWN     0x3D
#define VK_HOME     0x3E
#define VK_HELP     0x3F
#define VK_NFER     0x51
#define VK_F1       0x62
#define VK_F2       0x63
#define VK_F3       0x64
#define VK_F4       0x65
#define VK_F5       0x66
#define VK_F6       0x67
#define VK_F7       0x68
#define VK_F8       0x69
#define VK_F9       0x6A
#define VK_F10      0x6B

/* キーコードデータ操作マクロ */
#define KEYCODE(k)  (((k) >> 8) & 0x7F) /* 上位バイト = キーコード(スキャンコード) */
#define KEYDATA(k)  ((k) & 0xFF)        /* 下位バイト = ASCII/キーデータ */

#endif /* _VZ_H_ */
