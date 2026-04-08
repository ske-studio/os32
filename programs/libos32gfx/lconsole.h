/*
 * lconsole.h - OS32 Logical Console (差分仮想VRAM)
 * C89 compatible
 */
#ifndef OS32_LCONSOLE_H
#define OS32_LCONSOLE_H

/* コンソールサイズ */
#define LCONS_W 80
#define LCONS_H 25

/* 初期化とクリア */
void lcons_init(void);
void lcons_clear(void);

/* 論理VRAMへの文字出力 */
void lcons_putc(int x, int y, char ch, unsigned char attr);
void lcons_putkanji(int x, int y, unsigned short jis, unsigned char attr);

/* 論理VRAMの矩形塗りつぶし */
void lcons_fill_rect(int start_x, int start_y, int w, int h, char ch, unsigned char attr);

/* 物理VRAM(KCG)への差分同期 */
void lcons_sync_vram(void);

#endif /* OS32_LCONSOLE_H */
