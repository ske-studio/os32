#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock.py — OS32 ゲームUI のピクセル正確なモックアップ生成ライブラリ

実機と同じ制約でモックを描くためのホスト側レンダラ:
  - キャンバス 640x400、パレットは kernel/gfx/palette.c の PC-98 16色そのまま
  - microUI 用 8x8 フォント (programs/libos32gfx/text/gfx_font.c の font8x8)
  - kcg_draw_utf8 用 ANK 8x16 フォント (NP21/W の font.tmp から抽出)

描画APIは libos32gfx の名前に寄せてある。採用したモックは、この
スクリプトの描画呼び出しをほぼそのまま C へ移植できる。

使い方:
    from uimock import Screen
    s = Screen()
    s.fill_rect(0, 0, 640, 16, C_BLUE_D)
    s.text16(8, 0, "Susanoo", C_WHITE)
    s.save('mock.png', scale=1)
"""

import json
import os

try:
    from PIL import Image
except ImportError:
    raise SystemExit("Pillow が必要です: pip install pillow")

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(TOOLS_DIR)

SCREEN_W = 640
SCREEN_H = 400

# ---------------------------------------------------------------------------
# パレット — gfx/palette.c の default_palette (4bit/ch) をそのまま 8bit へ
# ---------------------------------------------------------------------------
_PAL4 = [
    (0,  0,  0),    # 0  黒
    (0,  0,  7),    # 1  青
    (7,  0,  0),    # 2  赤
    (7,  0,  7),    # 3  紫
    (0,  7,  0),    # 4  緑
    (0,  7,  7),    # 5  水色
    (7,  7,  0),    # 6  黄
    (7,  7,  7),    # 7  灰 (暗い白)
    (0,  0,  0),    # 8  黒(明) — 0 と同じ。事実上15色しかない
    (0,  0, 15),    # 9  明青
    (15, 0,  0),    # 10 明赤
    (15, 0, 15),    # 11 明紫
    (0, 15,  0),    # 12 明緑
    (0, 15, 15),    # 13 明水色
    (15, 15, 0),    # 14 明黄
    (15, 15, 15),   # 15 白
]
PALETTE = [(r * 17, g * 17, b * 17) for (r, g, b) in _PAL4]

# 色番号の別名 (実機コードと同じ番号)
C_BLACK    = 0
C_BLUE_D   = 1
C_RED_D    = 2
C_MAGENTA_D = 3
C_GREEN_D  = 4
C_CYAN_D   = 5
C_YELLOW_D = 6
C_GRAY     = 7
C_BLACK2   = 8
C_BLUE     = 9
C_RED      = 10
C_MAGENTA  = 11
C_GREEN    = 12
C_CYAN     = 13
C_YELLOW   = 14
C_WHITE    = 15


# ---------------------------------------------------------------------------
# フォント
# ---------------------------------------------------------------------------

def _load_font8x8():
    """gfx_font.c の font8x8 (0x20-0x7E の 95 文字) を読む"""
    path = os.path.join(TOOLS_DIR, 'uimock_font8x8.json')
    with open(path) as f:
        return json.load(f)


FONT8 = _load_font8x8()

# NP21/W が生成する font.tmp は 2048x2048 の 1bpp BMP。
# 先頭行に ANK 256 文字が 8x16 セルで横一列に並んでいる。
_FONT_TMP_CANDIDATES = [
    os.path.join(os.environ.get('NP21W_DIR',
                                '/mnt/c/Users/hight/Documents/np21w'), 'font.tmp'),
]


def _load_ank16():
    """ANK 8x16 フォントを [256][16] のビットマップ行データとして返す。
    font.tmp が無い環境では None (text16 が 8x8 の倍角描画にフォールバック)"""
    for path in _FONT_TMP_CANDIDATES:
        if not os.path.isfile(path):
            continue
        atlas = Image.open(path).convert('1')
        px = atlas.load()
        glyphs = []
        for code in range(256):
            rows = []
            ox = code * 8
            for y in range(16):
                bits = 0
                for x in range(8):
                    # 1bpp BMP は 0=黒(=文字), 255=白(=背景)
                    if px[ox + x, y] == 0:
                        bits |= 0x80 >> x
                rows.append(bits)
            glyphs.append(rows)
        return glyphs
    return None


ANK16 = _load_ank16()


# ---------------------------------------------------------------------------
# スクリーン
# ---------------------------------------------------------------------------

class Screen(object):
    """640x400 / 16色のインデックスバッファ。API は libos32gfx に寄せてある"""

    def __init__(self, bg=C_BLACK):
        self.w = SCREEN_W
        self.h = SCREEN_H
        self.buf = bytearray([bg]) * (self.w * self.h)

    # -- 基本描画 ----------------------------------------------------------

    def pixel(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.buf[y * self.w + x] = c

    def fill_rect(self, x, y, w, h, c):
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.w, x + w), min(self.h, y + h)
        for yy in range(y0, y1):
            base = yy * self.w
            self.buf[base + x0:base + x1] = bytes([c]) * (x1 - x0)

    def rect(self, x, y, w, h, c):
        """枠線のみ"""
        self.fill_rect(x, y, w, 1, c)
        self.fill_rect(x, y + h - 1, w, 1, c)
        self.fill_rect(x, y, 1, h, c)
        self.fill_rect(x + w - 1, y, 1, h, c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.pixel(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = err * 2
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def circle(self, cx, cy, r, c):
        x, y, d = r, 0, 1 - r
        while x >= y:
            for px, py in ((x, y), (y, x), (-x, y), (-y, x),
                           (x, -y), (y, -x), (-x, -y), (-y, -x)):
                self.pixel(cx + px, cy + py, c)
            y += 1
            if d < 0:
                d += 2 * y + 1
            else:
                x -= 1
                d += 2 * (y - x) + 1

    def fill_circle(self, cx, cy, r, c):
        for dy in range(-r, r + 1):
            span = int((r * r - dy * dy) ** 0.5)
            self.fill_rect(cx - span, cy + dy, span * 2 + 1, 1, c)

    # -- テキスト ----------------------------------------------------------

    def text8(self, x, y, s, fg, bg=None):
        """microUI と同じ 8x8 フォント。戻り値=描画幅"""
        cx = x
        for ch in s:
            code = ord(ch)
            if bg is not None:
                self.fill_rect(cx, y, 8, 8, bg)
            idx = code - 0x20
            if 0 <= idx < 95:
                glyph = FONT8[idx]
                for row in range(8):
                    bits = glyph[row]
                    for col in range(8):
                        if bits & (0x80 >> col):
                            self.pixel(cx + col, y + row, fg)
            cx += 8
        return cx - x

    def text16(self, x, y, s, fg, bg=None):
        """kcg_draw_utf8 と同じ ANK 8x16。戻り値=描画幅"""
        cx = x
        for ch in s:
            code = ord(ch)
            if bg is not None:
                self.fill_rect(cx, y, 8, 16, bg)
            if ANK16 is not None and code < 256:
                rows = ANK16[code]
                for row in range(16):
                    bits = rows[row]
                    for col in range(8):
                        if bits & (0x80 >> col):
                            self.pixel(cx + col, y + row, fg)
            else:
                # font.tmp が無い環境: 8x8 を縦2倍にして代用 (metrics は同じ)
                idx = code - 0x20
                if 0 <= idx < 95:
                    glyph = FONT8[idx]
                    for row in range(8):
                        bits = glyph[row]
                        for col in range(8):
                            if bits & (0x80 >> col):
                                self.pixel(cx + col, y + row * 2, fg)
                                self.pixel(cx + col, y + row * 2 + 1, fg)
            cx += 8
        return cx - x

    @staticmethod
    def text8_w(s):
        return len(s) * 8

    @staticmethod
    def text16_w(s):
        return len(s) * 8

    # -- 出力 --------------------------------------------------------------

    def to_image(self, scale=1):
        im = Image.frombytes('P', (self.w, self.h), bytes(self.buf))
        pal = []
        for (r, g, b) in PALETTE:
            pal += [r, g, b]
        pal += [0] * (768 - len(pal))
        im.putpalette(pal)
        if scale != 1:
            im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        return im

    def save(self, path, scale=1):
        self.to_image(scale).save(path)
        return path


# ---------------------------------------------------------------------------
# PC-98 RPG 風の共通パーツ
# ---------------------------------------------------------------------------

def win(s, x, y, w, h, body=C_BLUE_D, edge=C_WHITE, shadow=True):
    """二重罫線のウィンドウ枠。PC-98 RPG の定番。
    外枠(白) / 1dot 空け / 内枠(白) / 本体(濃紺)"""
    if shadow:
        s.fill_rect(x + 4, y + 4, w, h, C_BLACK)
    s.fill_rect(x, y, w, h, body)
    s.rect(x, y, w, h, edge)
    s.rect(x + 2, y + 2, w - 4, h - 4, edge)


def gauge(s, x, y, w, h, val, mx, fg, bg=C_BLACK, frame=C_WHITE):
    """HP などのゲージ。枠つき"""
    s.rect(x, y, w, h, frame)
    s.fill_rect(x + 1, y + 1, w - 2, h - 2, bg)
    if mx > 0:
        fw = int((w - 2) * max(0, min(val, mx)) / mx)
        if fw > 0:
            s.fill_rect(x + 1, y + 1, fw, h - 2, fg)


def cursor(s, x, y, c=C_YELLOW):
    """選択カーソル ▶ (8x8 相当を 8x16 行の中央に置く)"""
    for i in range(7):
        s.fill_rect(x, y + i, 1, 1, c)
        s.fill_rect(x, y + 13 - i, 1, 1, c)
    for i in range(7):
        s.line(x, y + i, x + 6 - i, y + i, c)
        s.line(x, y + 13 - i, x + 6 - i, y + 13 - i, c)


def clip_text(s_str, max_px, per_char=8):
    """幅に収まらない文字列を切り詰める (末尾に '.' を置く)。
    microUI の中央揃えで先頭が消える問題への対処を、モック上でも再現する"""
    n = max_px // per_char
    if len(s_str) <= n:
        return s_str
    if n <= 1:
        return s_str[:n]
    return s_str[:n - 1] + '.'
