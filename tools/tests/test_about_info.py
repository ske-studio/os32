"""About (userland/rust/about) の中身は `ver` と同じ — 両方をホストで回して突き合わせる。

ユーザー指示 (2026-09-29): About の表示内容は `ver` コマンドの表示内容と同じにする。

  - About 側: info.rs は KAPI にも GUI にも触らない純粋な部分なので、`#[path]` で
    そのまま取り込んでホストの rustc でビルドし、`ver_lines` の出力を得る。
  - ver 側: 実物の userland/shell/cmd_base.c を tools/tests/ver_about_host.c が
    #include し (1 行も写さない)、KernelAPI の贋物で `cmd_ver` を回した出力を得る。

同じ入力 (KAPI 版・Build・boot_image_info の戻りと中身) で、ver の各行から行頭の
字下げを落としたものが About の各行と 1 行ずつ一致すること、を入力の組を変えて見る。
どちらかの文言・行の並び・Commit / Image CRC を出す条件が変わったら落ちる。

併せて、lib.rs が写している BootImageInfo と KAPI の版の判定が正典
(sdk/include/os32/os32_kapi_shared.h、include/config.h) とずれていないか、行が窓に
収まるか (幅・高さ) を見る。エミュレータは使わない。

  python3 -B tools/tests/test_about_info.py [--mutate]

--mutate は**否定側**: info.rs と cmd_base.c の写しを書き換え (実物は読むだけ)、
この試験が RED になることを見る。記録: tools/tests/about_info_tdd.md
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutpar                                                   # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
# 変異試験は写しをこの 2 つの環境変数で差し込む (既定は実物)。
INFO_RS = pathlib.Path(os.environ.get('OS32_ABOUT_INFO_RS',
                                      ROOT / 'userland/rust/about/src/info.rs'))
CMD_BASE_C = pathlib.Path(os.environ.get('OS32_VER_CMD_BASE_C',
                                         ROOT / 'userland/shell/cmd_base.c'))
LIB_RS = ROOT / 'userland/rust/about/src/lib.rs'
SHARED_H = ROOT / 'sdk/include/os32/os32_kapi_shared.h'
CONFIG_H = ROOT / 'include/config.h'
HARNESS = ROOT / 'tools/tests/ver_about_host.c'

HOST_FLAGS = ['-std=gnu89', '-Wall', '-Wextra', '-Werror',
              '-Wdeclaration-after-statement',
              '-D__cdecl=', '-D__OS32_USERLAND__']
HOST_INC = ['-I' + str(ROOT / p) for p in
            ('.', 'include', 'sdk/include', 'sdk/include/os32',
             'userland/lib', 'userland/shell')]

# 引数なし: 固定の小試験の結果を 1 行ずつ。
# 引数 "ver" + 8 個: ver_about_host と同じ入力で ver_lines の行を出す。
RUST_MAIN = r'''
#[path = "%(info)s"]
#[allow(dead_code)]
mod info;
use info::*;

fn show(l: &Line) { println!("{}", String::from_utf8_lossy(l.as_bytes())); }
fn line<F: FnOnce(&mut Line)>(f: F) { let mut l = Line::new(); f(&mut l); show(&l); }

const CFG: &[u8] = include_bytes!("%(config)s");
const V: &[u8] = sys_version(CFG);

/* lib.rs と同じ詰め方: 32 バイトの build (末尾 NUL)、24 バイトの commit (末尾 NUL)。 */
fn fixed<const N: usize>(s: &str) -> [u8; N] {
    let mut b = [0u8; N];
    let t = s.as_bytes();
    let n = if t.len() < N - 1 { t.len() } else { N - 1 };
    b[..n].copy_from_slice(&t[..n]);
    b
}

fn ver(a: &[String]) {
    let kapi: u32 = a[0].parse().unwrap();
    let build: [u8; 32] = fixed(&a[1]);
    let rc: i32 = a[2].parse().unwrap();
    let commit: [u8; 24] = fixed(&a[7]);
    let b = BootImage {
        crc_valid: a[3].parse::<u32>().unwrap() != 0,
        crc: a[4].parse().unwrap(),
        size: a[5].parse().unwrap(),
        source: a[6].parse().unwrap(),
        commit: &commit,
    };
    const EMPTY: Line = Line::new();
    let mut out: [Line; NLINES_MAX] = [EMPTY; NLINES_MAX];
    let n = ver_lines(&mut out, V, kapi, &build, if boot_shown(kapi, rc) { Some(&b) } else { None });
    for l in &out[..n] { show(l); }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 && args[1] == "ver" {
        ver(&args[2..]);
        return;
    }
    /* sys_version */
    println!("{}", String::from_utf8_lossy(V));
    println!("{}", String::from_utf8_lossy(sys_version(b"#define SYS_VERSION_X \"9.9\"\n#define SYS_VERSION \"1.2\"\n")));
    println!("{}", String::from_utf8_lossy(sys_version(b"/* #define SYS_VERSION \"8.8\" */\n")));
    println!("{}", String::from_utf8_lossy(sys_version(b"#define SYS_VERSION \"\"\n")));
    println!("{}", String::from_utf8_lossy(sys_version(b"")));
    println!("{}", String::from_utf8_lossy(sys_version(b"#define SYS_VERSION\t\t\"3.4\"")));
    /* 打ち切り: LINE_MAX を超えた分は捨てる */
    line(|l| { l.s(&[b'x'; 100]); });
    line(|l| { l.hex8(0); l.s(b" "); l.hex8(0xffffffff); l.s(b" "); l.dec(0); l.s(b" "); l.dec(4294967295); });
    println!("{}", LINE_MAX);
    println!("{}", NLINES_MAX);
    println!("{}", VER_HW.len());
}
'''

# (kapi, build, boot_image_info の戻り, crc_valid, crc, size, source, commit)
CASES = [
    (68, 'Sep 29 2026 12:00:00', 0, 1, 0x00ab12cd, 123456, 2, '0f0bc25d'),
    (68, 'Sep 29 2026 12:00:00', 0, 1, 0x00ab12cd, 123456, 1, '0f0bc25d'),
    (68, 'Sep 29 2026 12:00:00', 0, 1, 0x00ab12cd, 123456, 0, '0f0bc25d'),
    (68, 'Sep 29 2026 12:00:00', 0, 1, 1, 0, 7, '0f0bc25d-dirty'),
    (68, 'Sep 29 2026 12:00:00', 0, 0, 0x00ab12cd, 123456, 2, '0f0bc25d'),
    (68, 'Sep 29 2026 12:00:00', 0, 1, 0x00ab12cd, 123456, 2, ''),
    (68, 'Sep 29 2026 12:00:00', -2, 1, 0x00ab12cd, 123456, 2, '0f0bc25d'),
    (65, 'Sep 29 2026 12:00:00', 0, 1, 0x00ab12cd, 123456, 2, '0f0bc25d'),
    (64, 'Sep 29 2026 12:00:00', 0, 1, 0x00ab12cd, 123456, 2, '0f0bc25d'),
    (0, '', 0, 1, 0x00ab12cd, 123456, 2, '0f0bc25d'),
    # 桁の最大 (Image CRC の行が最長の 50 桁)、build / commit はバッファより長い。
    (4294967295, 'Sep 29 2026 12:00:00 and-a-very-long-tail',
     0, 1, 0xffffffff, 4294967295, 2, '0123456789abcdef0123456789abcdef'),
]


def header_int(text, name):
    m = re.search(r'^#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)' % re.escape(name), text, re.M)
    if not m:
        raise AssertionError('%s not found' % name)
    return int(m.group(1), 0)


def rust_int(text, name):
    m = re.search(r'pub const %s: \w+ = (0x[0-9A-Fa-f_]+|\d+);' % re.escape(name), text)
    if not m:
        raise AssertionError('%s not found in info.rs' % name)
    return int(m.group(1).replace('_', ''), 0)


def unindent(v):
    """ver の出力から 2 行目以降の字下げ "  " だけを落とす (末尾の空白は残す)。"""
    return v[:1] + [ln[2:] if ln.startswith('  ') else ln for ln in v[1:]]


def case_args(c):
    return [str(v) for v in c]


class AboutMatchesVer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='os32-about-info-')
        d = pathlib.Path(cls.tmp.name)
        main = d / 'main.rs'
        main.write_text(RUST_MAIN % {'info': INFO_RS, 'config': CONFIG_H})
        cls.rs = d / 'aboutinfo'
        tc = subprocess.run(['rustup', 'toolchain', 'list'],
                            capture_output=True, text=True)
        name = tc.stdout.split('\n')[0].split(' ')[0]
        r = subprocess.run(['rustc', '+' + name, '--edition', '2021', '-O',
                            '-o', str(cls.rs), str(main)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError('rustc failed:\n' + r.stderr)
        cls.c = d / 'ver_about_host'
        r = subprocess.run(['gcc', *HOST_FLAGS, *HOST_INC,
                            '-DVER_CMD_BASE_C="%s"' % CMD_BASE_C,
                            str(HARNESS), '-o', str(cls.c)],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError('gcc failed:\n' + r.stderr)
        r = subprocess.run([str(cls.rs)], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError('run failed:\n' + r.stderr)
        cls.out = r.stdout.split('\n')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def about(self, c):
        r = subprocess.run([str(self.rs), 'ver', *case_args(c)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.split('\n')[:-1]

    def ver(self, c):
        r = subprocess.run([str(self.c), *case_args(c)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.split('\n')[:-1]

    # ---- ver と同じ ------------------------------------------------------

    def test_about_equals_ver(self):
        # ver の行 (1 行目以外は "  " の字下げ) の字下げを落とすと About の行になる。
        for c in CASES:
            with self.subTest(case=c):
                v = self.ver(c)
                self.assertGreater(len(v), 0)
                for ln in v[1:]:
                    self.assertTrue(ln.startswith('  '), ln)
                self.assertEqual(self.about(c), unindent(v))

    def test_about_lines_pinned(self):
        # 突き合わせだけだと両方が同じ向きに壊れても通る。代表の 1 組は文字列で固定する。
        version = self.out[0]
        self.assertEqual(self.about(CASES[0]), [
            'PC-9801 OS32 v%s (Ring3 Native)' % version,
            'CPU: Intel 386+ (Protected Mode + Paging)',
            'PIC: 8259A x2 (remapped to INT 20h+)',
            'PIT: 8254 @ 100Hz',
            'KBD: uPD8251A (IRQ1)',
            'SER: uPD8251A RS-232C (IRQ4)',
            'SND: YM2203 (OPN) FM3+SSG3',
            'GFX: 640x400x16 CPU direct',
            'API: v68',
            'Build: Sep 29 2026 12:00:00',
            'Commit: 0f0bc25d',
            'Image CRC: 00ab12cd (123456 bytes, HDD loader)',
        ])

    def test_boot_lines_conditions(self):
        # Commit / Image CRC は v65 以上かつ boot_image_info == 0 のときだけ。
        tails = {c: self.about(c)[10:] for c in CASES}
        self.assertEqual(tails[CASES[1]][1], 'Image CRC: 00ab12cd (123456 bytes, FD loader)')
        self.assertEqual(tails[CASES[2]][1], 'Image CRC: 00ab12cd (123456 bytes, ? loader)')
        self.assertEqual(tails[CASES[3]], ['Commit: 0f0bc25d-dirty',
                                           'Image CRC: 00000001 (0 bytes, ? loader)'])
        self.assertEqual(tails[CASES[4]][1], 'Image CRC: none (loader did not record)')
        self.assertEqual(tails[CASES[5]][0], 'Commit: ')
        self.assertEqual(tails[CASES[6]], [])
        self.assertEqual(len(tails[CASES[7]]), 2)
        self.assertEqual(tails[CASES[8]], [])
        self.assertEqual(tails[CASES[9]], [])

    def test_longest_lines_fit(self):
        # 桁の最大の組: 打ち切りで食い違わない (About が LINE_MAX で切れば一致しない)。
        c = CASES[-1]
        v = unindent(self.ver(c))
        line_max = int(self.out[8])
        self.assertLessEqual(max(len(ln) for ln in v), line_max)
        self.assertIn('Image CRC: ffffffff (4294967295 bytes, HDD loader)', v)
        self.assertEqual(len(v), int(self.out[9]))   # NLINES_MAX で足りる

    # ---- 部品 ------------------------------------------------------------

    def test_sys_version_from_config_h(self):
        m = re.search(r'^#define\s+SYS_VERSION\s+"([^"]+)"', CONFIG_H.read_text(), re.M)
        self.assertIsNotNone(m)
        self.assertEqual(self.out[0], m.group(1))

    def test_sys_version_edges(self):
        self.assertEqual(self.out[1:6], ['1.2', '?', '?', '?', '3.4'])

    def test_truncate_and_numbers(self):
        line_max = int(self.out[8])
        self.assertEqual(self.out[6], 'x' * line_max)
        self.assertEqual(self.out[7], '00000000 ffffffff 0 4294967295')

    # ---- 窓に収まる -------------------------------------------------------

    def test_line_fits_window_width(self):
        # 窓の外形の上限 (lib.rs の clamp の hi) から枠 (gshell wm.rs BORDER_W) × 2 と
        # column の余白 × 2 を引き、8px の半角で割った桁数を超える行は作らない。
        lib = LIB_RS.read_text()
        ww = int(re.search(r'let ww = clamp\(sw - 16, \d+, (\d+)\);', lib).group(1))
        pad = int(re.search(r'widget::column\((\d+), \d+\)', lib).group(1))
        wm = (ROOT / 'userland/gshell/src/wm.rs').read_text()
        border = int(re.search(r'pub const BORDER_W: i32 = (\d+);', wm).group(1))
        cols = (ww - 2 * border - 2 * pad) // 8
        self.assertLessEqual(rust_int(INFO_RS.read_text(), 'LINE_MAX'), cols)

    def test_lines_fit_window_height(self):
        # 最大の行数 (NLINES_MAX) + 区切り 2 本 + 伸びる余白 (0) + OK の段が、
        # 窓の高さの上限から枠 × 2・タイトルバー・column の余白 × 2 を引いた中に入る。
        lib = LIB_RS.read_text()
        wh = int(re.search(r'let wh = clamp\(sh - 40, \d+, (\d+)\);', lib).group(1))
        m = re.search(r'widget::column\((\d+), (\d+)\)', lib)
        pad, gap = int(m.group(1)), int(m.group(2))
        sep = int(re.search(r'widget::add\(root, gap, SizeSpec::Fixed\((\d+)\)\)', lib).group(1))
        bar = int(re.search(r'widget::add\(root, bar, SizeSpec::Fixed\((\d+)\)\)', lib).group(1))
        wm = (ROOT / 'userland/gshell/src/wm.rs').read_text()
        border = int(re.search(r'pub const BORDER_W: i32 = (\d+);', wm).group(1))
        title = int(re.search(r'pub const TITLEBAR_H: i32 = (\d+);', wm).group(1))
        n = int(self.out[9])
        items = n + 2 + 1 + 1                     # 行 + 区切り 2 + 余白 + OK の段
        need = 20 + (n - 1) * 16 + 2 * sep + bar + (items - 1) * gap + 2 * pad
        self.assertLessEqual(need + 2 * border + title, wh)
        # 640x400 (最小の画面) でもタスクバー 24px を残して上限の高さが取れる。
        self.assertLessEqual(wh, 400 - 40)

    # ---- 正典との一致 -----------------------------------------------------

    def test_constants_match_header(self):
        h = SHARED_H.read_text()
        r = INFO_RS.read_text()
        # BootImageInfo.source の値 (ヘッダはコメントで 1 = FD、2 = HDD と書く)。
        self.assertRegex(h, r'u8\s+source;\s*/\*\s*1 = FD ローダ、2 = HDD ローダ')
        self.assertEqual(rust_int(r, 'BOOT_SRC_FD'), 1)
        self.assertEqual(rust_int(r, 'BOOT_SRC_HDD'), 2)

    def test_boot_image_info_mirror(self):
        # lib.rs の写しがヘッダの並び (40 バイト、commit は BOOT_IMAGE_COMMIT_MAX) と一致するか。
        h = SHARED_H.read_text()
        commit_max = header_int(h, 'BOOT_IMAGE_COMMIT_MAX')
        m = re.search(r'typedef struct \{([^}]*)\} BootImageInfo;', h)
        self.assertIsNotNone(m)
        c_fields = re.findall(r'^\s*(u32|u16|u8|char)\s+(\w+)', m.group(1), re.M)
        self.assertEqual([f[1] for f in c_fields],
                         ['image_crc', 'image_size', 'crc_valid', 'source',
                          'reserved', 'commit', 'reserved2'])
        lib = LIB_RS.read_text()
        m = re.search(r'struct BootImageInfo \{([^}]*)\}', lib)
        self.assertIsNotNone(m)
        r_fields = re.findall(r'(\w+): ([\w\[\]; 0-9]+),', m.group(1))
        self.assertEqual([f[0] for f in r_fields], [f[1] for f in c_fields])
        self.assertIn('commit: [u8; %d]' % commit_max, m.group(1))
        self.assertIn('size_of::<BootImageInfo>() == 40', lib)
        # 試験の Rust 側の詰め方 (build 32 / commit 24) も lib.rs と同じ大きさ。
        self.assertIn('let mut build = [0u8; 32];', lib)
        self.assertIn('char build[32];', CMD_BASE_C.read_text())

    def test_kapi_gate_matches_header(self):
        # boot_image_info は KAPI v65 で入った (ヘッダのコメントが正典)。
        h = SHARED_H.read_text()
        self.assertIn('boot_image_info (KAPI v65', h)
        self.assertEqual(rust_int(INFO_RS.read_text(), 'KAPI_BOOT_IMAGE_INFO'), 65)
        # lib.rs は判定を info.rs から使う (自前の写しを持たない)。
        lib = LIB_RS.read_text()
        self.assertIn('info::boot_shown(', lib)
        self.assertIn('info::ver_lines(', lib)

    def test_deployed(self):
        # [V2] 起動できるバイナリは deploy.yaml に載っていること。
        self.assertIn('host: userland/system/about.bin',
                      (ROOT / 'userland/deploy.yaml').read_text())


# ---------------------------------------------------------------------------
#  否定側 (--mutate)。写しを書き換え、環境変数で差し込んで試験を回す。
# ---------------------------------------------------------------------------
INFO_REL = 'userland/rust/about/src/info.rs'
CMD_REL = 'userland/shell/cmd_base.c'

MUTATIONS = [
    # About 側 (info.rs)
    ('info-hw-text', INFO_REL, 'b"PIT: 8254 @ 100Hz"', 'b"PIT: 8254 @ 100 Hz"'),
    ('info-head-tail', INFO_REL, 'b" (Ring3 Native)"', 'b""'),
    ('info-api-label', INFO_REL, 'out.s(b"API: v")', 'out.s(b"KernelAPI: v")'),
    ('info-loader-swap', INFO_REL, 'if source == BOOT_SRC_HDD {', 'if source == BOOT_SRC_FD {'),
    ('info-crc-none', INFO_REL, 'out.s(b"none (loader did not record)")', 'out.s(b"none")'),
    ('info-commit-unknown', INFO_REL, 'out.s(b"Commit: ").s(commit);',
     'out.s(b"Commit: ").s(if commit.is_empty() || commit[0] == 0 '
     '{ b"unknown".as_slice() } else { commit });'),
    ('info-gate-ignores-rc', INFO_REL, 'kapi >= KAPI_BOOT_IMAGE_INFO && rc == 0',
     'kapi >= KAPI_BOOT_IMAGE_INFO'),
    ('info-gate-off-by-one', INFO_REL, 'pub const KAPI_BOOT_IMAGE_INFO: u32 = 65;',
     'pub const KAPI_BOOT_IMAGE_INFO: u32 = 64;'),
    ('info-crc-no-size', INFO_REL, '.dec(b.size)', '.dec(0)'),
    ('info-sysver-anywhere', INFO_REL, "if i == 0 || cfg[i - 1] == b'\\n' {", 'if true {'),
    # ver 側 (cmd_base.c) — ver だけ変えて About を直し忘れた、を捕まえる
    ('ver-hw-text', CMD_REL, '"  PIT: 8254 @ 100Hz\\n"', '"  PIT: 8254 @ 123Hz\\n"'),
    ('ver-new-line', CMD_REL, '"  GFX: 640x400x16 CPU direct\\n");',
     '"  GFX: 640x400x16 CPU direct\\n");\n'
     '    g_api->kprintf(ATTR_CYAN, "%s", "  FDC: uPD765A\\n");'),
    ('ver-gate', CMD_REL, 'g_api->version >= 65', 'g_api->version >= 60'),
    ('ver-crc-text', CMD_REL, '"  Image CRC: none (loader did not record)\\n"',
     '"  Image CRC: none\\n"'),
]


def one_mutation(item):
    """変異 1 本 (実物は読むだけ)。返り値は (印字する行, 見逃し 1 / 0)。"""
    name, rel, old, new = item
    original = (ROOT / rel).read_text(encoding='utf-8')
    if old not in original:
        return 'MUTATE %-22s SKIP (目印が見つからない)' % name, 1
    with tempfile.TemporaryDirectory(prefix='os32-about-mut-') as td:
        copy = pathlib.Path(td) / pathlib.Path(rel).name
        copy.write_text(original.replace(old, new, 1), encoding='utf-8')
        env = dict(os.environ)
        env['OS32_ABOUT_INFO_RS' if rel == INFO_REL else 'OS32_VER_CMD_BASE_C'] = str(copy)
        r = subprocess.run([sys.executable, '-B', str(pathlib.Path(__file__).resolve())],
                           env=env, capture_output=True, text=True, timeout=300)
    if r.returncode == 0:
        return 'MUTATE %-22s **GREEN のまま = 試験が見ていない**' % name, 1
    fails = len(re.findall(r'^(FAIL|ERROR):', r.stderr, re.M))
    return 'MUTATE %-22s RED (期待どおり落ちた: FAIL/ERROR %d 件)' % (name, fails), 0


if __name__ == '__main__':
    mutate = '--mutate' in sys.argv
    argv = [a for a in sys.argv if a != '--mutate']
    prog = unittest.main(argv=argv, exit=False)
    failed = not prog.result.wasSuccessful()
    if mutate:
        # 対照 (変異なしの写し) は GREEN、変異は全部 RED のはず。
        bad = mutpar.run_with_control(
            one_mutation, MUTATIONS,
            ('control', INFO_REL, 'pub const LINE_MAX', 'pub const LINE_MAX'))
        failed = failed or bad != 0
    sys.exit(1 if failed else 0)
