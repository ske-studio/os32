"""Host-only tests: all file inputs are synthetic temporary fixtures."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / 'np21w_ini.py'
if SCRIPT.exists():
    spec = importlib.util.spec_from_file_location('np21w_ini', SCRIPT)
    ini = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ini)
else:
    ini = None

RAW = (b'; opaque \x82\xa0\xff\r\n[NekoProject21]\r\n'
       b' USEGD5430 = false \t; keep\r\nGD5430TYPE=91\nUSEPEGCP=false\n'
       b'private=DO_NOT_PRINT\r\n[other]\r\nUSEGD5430=false')
CHANGES = {'USEGD5430': 'true'}
EXPECTED = RAW.replace(b'= false', b'= true', 1)


class Pegc(unittest.TestCase):
    """USEPEGCP gates np2cfg.usepegcplane -> pegc.enable (win9x/ini.cpp:687,
    io/pegc.c:375). Same [NekoProject21] table and PFTYPE_BOOL as USEGD5430."""

    def test_pegc_on_off_round_trip(self):
        on, diff = ini.transform(RAW, {'USEPEGCP': 'true'})
        self.assertEqual(diff, ['USEPEGCP: false -> true'])
        self.assertEqual(on, RAW.replace(b'USEPEGCP=false', b'USEPEGCP=true'))
        back, diff = ini.transform(on, {'USEPEGCP': 'false'})
        self.assertEqual((back, diff), (RAW, ['USEPEGCP: true -> false']))

    def test_pegc_is_independent_of_cirrus(self):
        out, diff = ini.transform(RAW, {'USEPEGCP': 'true'})
        self.assertIn(b' USEGD5430 = false \t; keep', out)
        self.assertIn(b'GD5430TYPE=91', out)
        self.assertEqual(diff, ['USEPEGCP: false -> true'])

    def test_rejects_bad_pegc_values(self):
        for value in ('1', 'True', 'yes', '', 'true\nprivate=x'):
            with self.assertRaises(ini.IniError):
                ini.transform(RAW, {'USEPEGCP': value})

    def test_missing_or_duplicate_pegc_field_fails_closed(self):
        for raw in (RAW.replace(b'USEPEGCP=false\n', b''),
                    RAW.replace(b'USEPEGCP=false', b'USEPEGCP=false\nusepegcp=false'),
                    RAW.replace(b'USEPEGCP=false', b'USEPEGCP=unknown')):
            with self.assertRaises(ini.IniError):
                ini.transform(raw, {'USEGD5430': 'true'})


class Availability(unittest.TestCase):
    def test_implementation_exists(self):
        self.assertTrue(SCRIPT.is_file(), 'np21w_ini.py must implement the tested contract')


@unittest.skipIf(ini is None, 'implementation not yet written (assertion RED)')
class Transformation(unittest.TestCase):
    def test_preserves_unrelated_bytes(self):
        result, diff = ini.transform(RAW, CHANGES)
        self.assertEqual(result, EXPECTED)
        self.assertEqual(diff, ['USEGD5430: false -> true'])

    def test_bom_newlines_and_no_final_newline(self):
        for prefix in (b'', b'\xef\xbb\xbf'):
            for newline in (b'\r\n', b'\n', b'\r'):
                raw = prefix + newline.join((b'[NekoProject21]', b'USEGD5430=false',
                                             b'GD5430TYPE=91', b'USEPEGCP=false'))
                result, _ = ini.transform(raw, CHANGES)
                self.assertEqual(result, raw.replace(b'USEGD5430=false', b'USEGD5430=true'))

    def test_noop_and_disable(self):
        self.assertEqual(ini.transform(EXPECTED, CHANGES), (EXPECTED, []))
        self.assertEqual(ini.transform(EXPECTED, {'USEGD5430': 'false'})[0], RAW)

    def test_allowlist_rejects_unproven_values(self):
        for changes in ({'WAB_ANSW': '1'}, {'GD5430TYPE': '0x5b'},
                        {'GD5430TYPE': '65535'}, {'USEGD5430': '1'},
                        {'USEGD5430': 'true\nprivate=x'}, {}, {'usegd5430': 'true'}):
            with self.subTest(changes=changes), self.assertRaises(ini.IniError):
                ini.transform(RAW, changes)

    def test_missing_duplicate_and_malformed(self):
        for raw in (b'[other]\nUSEGD5430=false\n',
                    RAW.replace(b'GD5430TYPE=91', b''),
                    RAW.replace(b'GD5430TYPE=91', b'GD5430TYPE=91\ngd5430type=91'),
                    RAW + b'\n[NekoProject21]\n',
                    RAW.replace(b'GD5430TYPE=91', b'GD5430TYPE=91\nusegd5430=false'),
                    RAW.replace(b'GD5430TYPE=91', b'GD5430TYPE=91\n[broken'),
                    RAW.replace(b'GD5430TYPE=91', b'GD5430TYPE=unknown'),
                    b'\xff\xfe' + RAW, RAW + b'\x00'):
            with self.subTest(raw=raw[:24]), self.assertRaises(ini.IniError):
                ini.transform(raw, CHANGES)


@unittest.skipIf(ini is None, 'implementation not yet written (assertion RED)')
class OfflineFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'synthetic.snapshot'
        self.source.write_bytes(RAW)

    def test_unique_backup_atomic_readback_and_restore(self):
        a = ini.prepare(self.source, self.root, CHANGES)
        b = ini.prepare(self.source, self.root, CHANGES)
        self.assertNotEqual(a, b)
        self.assertEqual(self.source.read_bytes(), RAW)
        self.assertEqual((a / 'original.bin').read_bytes(), RAW)
        self.assertEqual((a / 'prepared.bin').read_bytes(), EXPECTED)
        self.assertEqual(ini.restore(a), ['USEGD5430: true -> false'])
        self.assertEqual((a / 'prepared.bin').read_bytes(), EXPECTED)
        ini.restore(a, apply=True)
        self.assertEqual((a / 'prepared.bin').read_bytes(), RAW)
        self.assertEqual((a / 'original.bin').read_bytes(), RAW)
        with self.assertRaises(ini.IniError):
            ini.restore(a, apply=True)

    def test_intervening_modification_refuses_restore(self):
        for name in ('prepared.bin', 'original.bin', 'receipt.json'):
            bundle = ini.prepare(self.source, self.root, CHANGES)
            path = bundle / name
            path.write_bytes(path.read_bytes() + b'changed')
            before = (bundle / 'prepared.bin').read_bytes()
            with self.subTest(name=name), self.assertRaises(ini.IniError):
                ini.restore(bundle, apply=True)
            self.assertEqual((bundle / 'prepared.bin').read_bytes(), before)

    def test_same_bytes_replacement_refuses_restore(self):
        bundle = ini.prepare(self.source, self.root, CHANGES)
        replacement = bundle / 'replacement'
        replacement.write_bytes(EXPECTED)
        replacement.replace(bundle / 'prepared.bin')
        with self.assertRaises(ini.IniError):
            ini.restore(bundle, apply=True)
        self.assertEqual((bundle / 'prepared.bin').read_bytes(), EXPECTED)

    def test_receipt_cannot_authorize_unrelated_rewrite(self):
        bundle = ini.prepare(self.source, self.root, CHANGES)
        path = bundle / 'receipt.json'
        receipt = json.loads(path.read_bytes())
        receipt['changes'] = {'private': 'replacement'}
        path.write_text(json.dumps(receipt))
        with self.assertRaises(ini.IniError):
            ini.restore(bundle, apply=True)
        self.assertEqual((bundle / 'prepared.bin').read_bytes(), EXPECTED)

    def test_restore_cli_defaults_to_dry_run(self):
        bundle = ini.prepare(self.source, self.root, CHANGES)
        code, out, _ = self.run_cli(['restore', str(bundle)])
        self.assertEqual((code, out), (0, 'USEGD5430: true -> false\n'))
        self.assertEqual((bundle / 'prepared.bin').read_bytes(), EXPECTED)
        code, _, _ = self.run_cli(['restore', str(bundle), '--apply'])
        self.assertEqual(code, 0)
        self.assertEqual((bundle / 'prepared.bin').read_bytes(), RAW)

    def test_invalid_preparation_creates_nothing(self):
        with self.assertRaises(ini.IniError):
            ini.prepare(self.source, self.root, {'GD5430TYPE': '0'})
        self.assertEqual(list(self.root.iterdir()), [self.source])

    def test_restore_rechecks_immediately_before_replace(self):
        bundle = ini.prepare(self.source, self.root, CHANGES)
        real = ini._new_file
        def intervene(fd, name, data):
            real(fd, name, data)
            (bundle / 'prepared.bin').write_bytes(b'intervening bytes')
        with mock.patch.object(ini, '_new_file', side_effect=intervene):
            with self.assertRaises(ini.IniError):
                ini.restore(bundle, apply=True)
        self.assertEqual((bundle / 'prepared.bin').read_bytes(), b'intervening bytes')
        self.assertEqual((bundle / 'original.bin').read_bytes(), RAW)

    def test_symlinks_rejected_including_parents(self):
        link = self.root / 'link'
        link.symlink_to(self.source)
        with self.assertRaises(ini.IniError):
            ini.prepare(link, self.root, CHANGES)
        link.unlink()
        link.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ini.IniError):
            ini.prepare(link / self.source.name, self.root, CHANGES)
        with self.assertRaises(ini.IniError):
            ini.prepare(self.source, link, CHANGES)
        bundle = ini.prepare(self.source, self.root, CHANGES)
        (bundle / 'prepared.bin').unlink()
        (bundle / 'prepared.bin').symlink_to(self.source)
        with self.assertRaises(ini.IniError):
            ini.restore(bundle, apply=True)
        self.assertEqual(self.source.read_bytes(), RAW)

    def test_hardlink_and_nonregular_rejected(self):
        os.link(self.source, self.root / 'hard')
        with self.assertRaises(ini.IniError):
            ini.prepare(self.source, self.root, CHANGES)
        with self.assertRaises(ini.IniError):
            ini.prepare(self.root, self.root, CHANGES)

    def test_atomic_failure_preserves_backup_and_source(self):
        with mock.patch.object(ini.os, 'replace', side_effect=OSError('synthetic failure')):
            with self.assertRaises(OSError):
                ini.prepare(self.source, self.root, CHANGES)
        bundle, = self.root.glob('np21w-offline-*')
        self.assertEqual((bundle / 'original.bin').read_bytes(), RAW)
        self.assertFalse((bundle / 'prepared.bin').exists())
        self.assertEqual(self.source.read_bytes(), RAW)

    def test_readback_failure_is_reported(self):
        real = ini.os.replace
        def corrupt(src, dst, **kwargs):
            real(src, dst, **kwargs)
            fd = os.open(dst, os.O_WRONLY, dir_fd=kwargs['dst_dir_fd'])
            try:
                os.write(fd, b'!')
            finally:
                os.close(fd)
        with mock.patch.object(ini.os, 'replace', side_effect=corrupt):
            with self.assertRaises(ini.IniError):
                ini.prepare(self.source, self.root, CHANGES)
        self.assertEqual(self.source.read_bytes(), RAW)

    def run_cli(self, args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ini.main(args)
        self.assertNotIn('DO_NOT_PRINT', out.getvalue() + err.getvalue())
        return code, out.getvalue(), err.getvalue()

    def test_cli_dry_run_and_explicit_offline_apply(self):
        args = ['prepare', str(self.source), '--set', 'USEGD5430=true']
        code, out, _ = self.run_cli(args)
        self.assertEqual(code, 0)
        self.assertEqual(out, 'USEGD5430: false -> true\n')
        self.assertEqual(list(self.root.iterdir()), [self.source])
        code, _, _ = self.run_cli(args + ['--output', str(self.root), '--apply'])
        self.assertEqual(code, 0)
        self.assertEqual(self.source.read_bytes(), RAW)

    def test_live_apply_fails_before_any_read(self):
        with mock.patch.object(ini, 'read_snapshot', side_effect=AssertionError('must not read')):
            code, _, err = self.run_cli(['prepare', 'not-read.ini', '--set',
                                          'USEGD5430=true', '--apply'])
        self.assertEqual(code, 2)
        self.assertIn('verifier', err)

    def test_repeated_assignment_rejected(self):
        code, _, _ = self.run_cli(['prepare', str(self.source), '--set',
                                   'USEGD5430=true', '--set', 'USEGD5430=false'])
        self.assertEqual(code, 2)


if __name__ == '__main__':
    unittest.main()
