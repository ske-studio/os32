#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
subset_font.py — IPAexゴシックからサブセットTTFを生成する

JIS第1水準 + JIS第2水準 + ASCII + かな + 記号を含むサブセットを作成。
fonttools (pyftsubset) が必要: pip install fonttools brotli

使用方法:
    python3 tools/subset_font.py [入力TTF] [出力TTF]

デフォルト:
    入力: assets/fonts/ipaexg.ttf
    出力: assets/fonts/ipaexg_subset.ttf
"""

import sys
import os
import struct

# ======================================================================
#  JIS X 0208 → Unicode 変換テーブル (JIS第1水準 + 第2水準)
#
#  JIS第1水準: 区点 0101-4794 (約2,965字)
#  JIS第2水準: 区点 4801-8406 (約3,390字)
#  合計: 約6,355字
# ======================================================================

def jis_to_unicode_codepoints():
    """
    JIS X 0208 の区点コードをPythonの codec で Unicode に変換。
    iso2022_jp codec 経由で全区点をスキャンする。
    """
    codepoints = set()

    for ku in range(1, 95):  # 区 1-94
        for ten in range(1, 95):  # 点 1-94
            # JIS第1水準: 区1-47, JIS第2水準: 区48-84
            if ku > 84:
                continue

            # JIS X 0208 のバイト列 (区+0x20, 点+0x20 → 0x21-0x7E)
            jis_hi = ku + 0x20
            jis_lo = ten + 0x20

            # ISO-2022-JP エスケープシーケンスでエンコードしデコード
            try:
                jis_bytes = b'\x1b$B' + bytes([jis_hi, jis_lo]) + b'\x1b(B'
                text = jis_bytes.decode('iso2022_jp')
                for ch in text:
                    cp = ord(ch)
                    if cp >= 0x80:  # ASCII以外のみ
                        codepoints.add(cp)
            except (UnicodeDecodeError, ValueError):
                pass

    return codepoints


def build_unicode_ranges(minimal=False):
    """サブセット対象の全 Unicode コードポイント集合を構築する"""
    codepoints = set()

    # ASCII (U+0020-007E)
    for cp in range(0x0020, 0x007F):
        codepoints.add(cp)

    # 全角記号 (U+3000-303F)
    for cp in range(0x3000, 0x3040):
        codepoints.add(cp)

    # ひらがな (U+3040-309F)
    for cp in range(0x3040, 0x30A0):
        codepoints.add(cp)

    # カタカナ (U+30A0-30FF)
    for cp in range(0x30A0, 0x3100):
        codepoints.add(cp)

    # 半角カナ (U+FF61-FF9F)
    for cp in range(0xFF61, 0xFFA0):
        codepoints.add(cp)

    if not minimal:
        # JIS第1水準 + JIS第2水準 (iso2022_jp codec経由)
        jis_cps = jis_to_unicode_codepoints()
        codepoints.update(jis_cps)

    return codepoints


def main():
    # パス解決
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)  # src/os32/

    input_ttf = os.path.join(project_dir, 'assets', 'fonts', 'ipaexg.ttf')
    output_ttf = os.path.join(project_dir, 'assets', 'fonts', 'ipaexg_subset.ttf')
    minimal = False

    args = sys.argv[1:]
    if '--minimal' in args:
        minimal = True
        args.remove('--minimal')
    if len(args) >= 1:
        input_ttf = args[0]
    if len(args) >= 2:
        output_ttf = args[1]

    if not os.path.exists(input_ttf):
        print("ERROR: 入力フォントが見つかりません: %s" % input_ttf)
        print("IPAexゴシック (ipaexg.ttf) を assets/fonts/ に配置してください。")
        print("ダウンロード: https://moji.or.jp/ipafont/ipaex00401/")
        sys.exit(1)

    try:
        from fontTools.subset import Subsetter, Options
        from fontTools.ttLib import TTFont
    except ImportError:
        print("ERROR: fonttools が必要です。")
        print("  pip install fonttools brotli")
        sys.exit(1)

    # Unicode コードポイント集合を構築
    if minimal:
        print("モード: ミニマル (ASCII + かな + 記号)")
    else:
        print("モード: フル (JIS第1・第2水準含む)")
    codepoints = build_unicode_ranges(minimal=minimal)
    print("サブセット対象: %d 文字" % len(codepoints))

    # unicodes 文字列リストを作成
    unicodes_str = ','.join('U+%04X' % cp for cp in sorted(codepoints))

    # フォント読み込み
    print("入力フォント: %s" % input_ttf)
    font = TTFont(input_ttf)

    # サブセット実行
    options = Options()
    options.flavor = None  # TTF形式のまま (WOFF2にしない)
    options.desubroutinize = True  # CFFの場合のサブルーチン展開

    subsetter = Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    # 出力ディレクトリ作成
    os.makedirs(os.path.dirname(output_ttf), exist_ok=True)

    # 保存
    font.save(output_ttf)

    # サイズ報告
    input_size = os.path.getsize(input_ttf)
    output_size = os.path.getsize(output_ttf)
    print("出力フォント: %s" % output_ttf)
    print("元サイズ: %d bytes (%.1f KB)" % (input_size, input_size / 1024.0))
    print("サブセット: %d bytes (%.1f KB)" % (output_size, output_size / 1024.0))
    print("削減率: %.1f%%" % ((1.0 - float(output_size) / input_size) * 100))


if __name__ == '__main__':
    main()
