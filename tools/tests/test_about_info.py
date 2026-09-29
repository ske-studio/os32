"""About (userland/rust/about) の行の組み立て — info.rs をホストで回す。

info.rs は KAPI にも GUI にも触らない純粋な部分なので、`#[path]` で
そのまま取り込んでホストの rustc でビルドし、数値 → 表示の文字列を突き合わせる。
併せて、info.rs と lib.rs が写している定数・構造体が正典
(sdk/include/os32/os32_kapi_shared.h、include/config.h) とずれていないかを見る。
エミュレータは使わない。
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
INFO_RS = ROOT / 'userland/rust/about/src/info.rs'
LIB_RS = ROOT / 'userland/rust/about/src/lib.rs'
SHARED_H = ROOT / 'sdk/include/os32/os32_kapi_shared.h'
CONFIG_H = ROOT / 'include/config.h'

# 各行を 1 行ずつ出す。区切りは "|" (試験の期待値と同じ順)。
RUST_MAIN = r'''
#[path = "%(info)s"]
#[allow(dead_code)]
mod info;
use info::*;

fn show(l: &Line) { println!("{}", String::from_utf8_lossy(l.as_bytes())); }
fn line<F: FnOnce(&mut Line)>(f: F) { let mut l = Line::new(); f(&mut l); show(&l); }

fn cpu(class: u8, vendor: &[u8], fam: u32, model: u32, brand: &[u8]) -> Cpu {
    let mut c = Cpu::empty();
    c.class = class;
    c.vendor[..vendor.len()].copy_from_slice(vendor);
    c.family = fam;
    c.model = model;
    c.brand[..brand.len()].copy_from_slice(brand);
    c
}

const CFG: &[u8] = include_bytes!("%(config)s");
const V: &[u8] = sys_version(CFG);

fn main() {
    /* sys_version */
    println!("{}", String::from_utf8_lossy(V));
    println!("{}", String::from_utf8_lossy(sys_version(b"#define SYS_VERSION_X \"9.9\"\n#define SYS_VERSION \"1.2\"\n")));
    println!("{}", String::from_utf8_lossy(sys_version(b"/* #define SYS_VERSION \"8.8\" */\n")));
    println!("{}", String::from_utf8_lossy(sys_version(b"#define SYS_VERSION \"\"\n")));
    println!("{}", String::from_utf8_lossy(sys_version(b"")));
    println!("{}", String::from_utf8_lossy(sys_version(b"#define SYS_VERSION\t\t\"3.4\"")));
    /* 各行 */
    line(|l| title_line(l, b"2.0"));
    line(|l| build_line(l, b"Sep 29 2026 12:00:00\0garbage"));
    line(|l| commit_line(l, true, b"0f0bc25d\0"));
    line(|l| commit_line(l, true, b"\0"));
    line(|l| commit_line(l, false, b"0f0bc25d\0"));
    line(|l| crc_line(l, true, true, 0x00ab12cd, 123456));
    line(|l| crc_line(l, true, false, 0x00ab12cd, 123456));
    line(|l| crc_line(l, false, true, 0x00ab12cd, 123456));
    line(|l| kapi_line(l, 68));
    line(|l| mem_line(l, 15360));
    line(|l| heap_line(l, 300 * 1024 + 5, 512 * 1024));
    line(|l| display_line(l, 640, 400, 4, 0, 0));
    line(|l| display_line(l, 640, 480, 8, GFX_FMT_PACKED8, 0));
    line(|l| display_line(l, 800, 600, 8, GFX_FMT_PACKED8, GFX_CAP_HW_BLT));
    line(|l| display_line(l, 640, 480, 8, GFX_FMT_PACKED8, GFX_CAP_HW_FILL));
    line(|l| display_line(l, 640, 400, 0, 0, 0));
    line(|l| display_line(l, 640, 400, 32, 0, 0));
    line(|l| boot_line(l, true, BOOT_SRC_HDD));
    line(|l| boot_line(l, true, BOOT_SRC_FD));
    line(|l| boot_line(l, true, 0));
    line(|l| boot_line(l, false, BOOT_SRC_HDD));
    line(|l| cpu_line(l, &cpu(3, b"", 0, 0, b"")));
    line(|l| cpu_line(l, &cpu(4, b"", 0, 0, b"")));
    line(|l| cpu_line(l, &cpu(0, b"GenuineIntel", 5, 2, b"")));
    line(|l| cpu_line(l, &cpu(0, b"GenuineIntel", 15, 2, b"      Intel(R) Pentium(R) 4 CPU 2.40GHz")));
    line(|l| cpu_line(l, &cpu(0, b"AuthenticAMD", 5, 8, b"                                               ")));
    /* 打ち切り: LINE_MAX を超えた分は捨てる */
    line(|l| { l.s(&[b'x'; 100]); });
    line(|l| { l.hex8(0); l.s(b" "); l.hex8(0xffffffff); l.s(b" "); l.dec(0); l.s(b" "); l.dec(4294967295); });
    /* family_model */
    for eax in [0x0000_0543u32, 0x0000_0633, 0x0001_06a5, 0x0010_0f29, 0x0000_0f41] {
        let (f, m) = family_model(eax);
        println!("{} {}", f, m);
    }
    /* colors */
    println!("{} {} {} {}", colors(1), colors(4), colors(8), colors(16));
    println!("{}", LINE_MAX);
}
'''


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


class AboutInfo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='os32-about-info-')
        d = pathlib.Path(cls.tmp.name)
        main = d / 'main.rs'
        main.write_text(RUST_MAIN % {'info': INFO_RS, 'config': CONFIG_H})
        exe = d / 'aboutinfo'
        tc = subprocess.run(['rustup', 'toolchain', 'list'],
                            capture_output=True, text=True)
        name = tc.stdout.split('\n')[0].split(' ')[0]
        r = subprocess.run(['rustc', '+' + name, '--edition', '2021', '-O',
                            '-o', str(exe), str(main)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError('rustc failed:\n' + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError('run failed:\n' + r.stderr)
        cls.out = r.stdout.split('\n')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_sys_version_from_config_h(self):
        m = re.search(r'^#define\s+SYS_VERSION\s+"([^"]+)"', CONFIG_H.read_text(), re.M)
        self.assertIsNotNone(m)
        self.assertEqual(self.out[0], m.group(1))

    def test_sys_version_edges(self):
        self.assertEqual(self.out[1:6], ['1.2', '?', '?', '?', '3.4'])

    def test_lines(self):
        want = [
            'OS32 v2.0',
            'Build: Sep 29 2026 12:00:00',
            'Commit: 0f0bc25d',
            'Commit: unknown',
            'Commit: unknown',
            'Image CRC: 00ab12cd (123456 bytes)',
            'Image CRC: none',
            'Image CRC: none',
            'KernelAPI: v68',
            'Memory: 15360 KB (15 MB)',
            'Kernel heap: 300 KB free of 512 KB',
            'Display: PC-98 640x400, 16 colors',
            'Display: PEGC 640x480, 256 colors',
            'Display: Cirrus 800x600, 256 colors',
            'Display: Cirrus 640x480, 256 colors',
            'Display: PC-98 640x400, ? colors',
            'Display: PC-98 640x400, ? colors',
            'Boot: HDD',
            'Boot: FD',
            'Boot: unknown',
            'Boot: unknown',
            'CPU: 386 (no CPUID)',
            'CPU: 486 (no CPUID)',
            'CPU: GenuineIntel family 5 model 2',
            'CPU: Intel(R) Pentium(R) 4 CPU 2.40GHz',
            'CPU: AuthenticAMD family 5 model 8',
        ]
        self.assertEqual(self.out[6:6 + len(want)], want)

    def test_truncate_and_numbers(self):
        base = 6 + 26
        line_max = int(self.out[base + 2 + 5 + 1])
        self.assertEqual(self.out[base], 'x' * line_max)
        self.assertEqual(self.out[base + 1], '00000000 ffffffff 0 4294967295')

    def test_family_model(self):
        base = 6 + 26 + 2
        self.assertEqual(self.out[base:base + 5],
                         ['5 4', '6 3', '6 26', '16 2', '15 4'])

    def test_colors(self):
        self.assertEqual(self.out[6 + 26 + 2 + 5], '2 16 256 65536')

    def test_line_fits_window(self):
        # 窓の外形の上限 (lib.rs の clamp の hi) から枠 (gshell wm.rs BORDER_W) × 2 と
        # column の余白 × 2 を引き、8px の半角で割った桁数を超える行は作らない。
        lib = LIB_RS.read_text()
        ww = int(re.search(r'let ww = clamp\(sw - 16, \d+, (\d+)\);', lib).group(1))
        pad = int(re.search(r'widget::column\((\d+), \d+\)', lib).group(1))
        wm = (ROOT / 'userland/gshell/src/wm.rs').read_text()
        border = int(re.search(r'pub const BORDER_W: i32 = (\d+);', wm).group(1))
        cols = (ww - 2 * border - 2 * pad) // 8
        self.assertLessEqual(rust_int(INFO_RS.read_text(), 'LINE_MAX'), cols)
        # 最長の行 (ブランド文字列は 48 バイトの NUL 終端 = 47 桁) が切れずに入る。
        self.assertGreaterEqual(rust_int(INFO_RS.read_text(), 'LINE_MAX'), len('CPU: ') + 47)

    def test_constants_match_header(self):
        h = SHARED_H.read_text()
        r = INFO_RS.read_text()
        for name in ('GFX_FMT_PACKED8', 'GFX_CAP_HW_FILL', 'GFX_CAP_HW_BLT'):
            self.assertEqual(rust_int(r, name), header_int(h, name), name)
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

    def test_kapi_gate_matches_header(self):
        # boot_image_info は KAPI v65 で入った (ヘッダのコメントが正典)。
        h = SHARED_H.read_text()
        self.assertIn('boot_image_info (KAPI v65', h)
        self.assertIn('const KAPI_BOOT_IMAGE_INFO: u32 = 65;', LIB_RS.read_text())

    def test_deployed(self):
        # [V2] 起動できるバイナリは deploy.yaml に載っていること。
        self.assertIn('host: userland/system/about.bin',
                      (ROOT / 'userland/deploy.yaml').read_text())


if __name__ == '__main__':
    unittest.main()
