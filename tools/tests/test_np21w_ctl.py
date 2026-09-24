"""Host-only tests for tools/np21w_ctl.py.

Windows 側の操作 (tasklist / taskkill / PowerShell) は FakeOps に、時計と sleep は
FakeClock に差し替える。実プロセス・実 ini・ネットワークには触れない。
"""
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import np21w_ctl as ctl  # noqa: E402


class FakeClock(object):
    def __init__(self):
        self.now = 0.0
        self.slept = []

    def clock(self):
        return self.now

    def sleep(self, sec):
        self.slept.append(sec)
        self.now += sec


class FakeOps(object):
    """procs: 呼ぶたびに 1 つずつ消費するプロセス一覧の列 (最後の値を保つ)。
    locks: {win_path: 何回目の probe まで locked か}。missing: 無いファイル。"""

    def __init__(self, procs=None, locks=None, missing=(), clock=None,
                 lock_seq=None, dies_after=None):
        self.procs = list(procs or [[]])
        self.locks = dict(locks or {})
        # lock_seq: {win_path: 'FLLF...'} — probe n 回目の状態 (F=free, L=locked)。
        # 尽きたら最後の文字を保つ
        self.lock_seq = dict(lock_seq or {})
        # dies_after: 起動後の list_processes を何回目まで起動したプロセスを返すか
        self.dies_after = dies_after
        self.after_start = 0
        self.missing = set(missing)
        self.probes = []
        self.killed = []
        self.started = []
        self.clock = clock

    def list_processes(self):
        if self.started:
            self.after_start += 1
            if self.dies_after is not None and self.after_start > self.dies_after:
                return []
            return [('np21x64w.exe', 4242)]
        if len(self.procs) > 1:
            return self.procs.pop(0)
        return self.procs[0]

    def kill(self, pid):
        self.killed.append(pid)

    def probe(self, paths):
        self.probes.append(list(paths))
        n = len(self.probes)
        out = {}
        for p in paths:
            if p in self.missing:
                out[p] = 'missing'
            elif p in self.lock_seq:
                seq = self.lock_seq[p]
                out[p] = 'locked' if seq[min(n, len(seq)) - 1] == 'L' else 'free'
            elif self.locks.get(p, 0) >= n:
                out[p] = 'locked'
            else:
                out[p] = 'free'
        return out

    def start(self, exe, args, cwd):
        self.started.append((exe, list(args), cwd))
        return 4242


WIN = 'C:\\NP21'
INI_TEXT = (
    '[NekoProject21]\r\n'
    'FDD1FILE=C:\\NP21\\os32_boot.d88\r\n'
    'FDD2FILE=\r\n'
    'HDD1FILE=C:\\NP21\\os32.nhd\r\n'
    'HDD2FILE=\r\n'
    'CD1_FILE=C:\\NP21\\os32_install.iso\r\n'
    'CD2_FILE=\r\n'
    'CDfolder=C:\\Users\\x\\\u30c9\u30ad\u30e5\u30e1\u30f3\u30c8\\a.iso\r\n'
)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        for name in ('np21x64w.exe', 'os32.nhd', 'os32_boot.d88'):
            open(os.path.join(self.dir, name), 'wb').close()
        with open(os.path.join(self.dir, 'test.ini'), 'wb') as f:
            f.write(INI_TEXT.encode('cp932'))
        self.fc = FakeClock()
        self.out = io.StringIO()
        self.err = io.StringIO()

    def tearDown(self):
        self.tmp.cleanup()

    def make(self, ops, tvram=None, status_fn=None):
        self.ops = ops
        return ctl.Ctl(ctl.Paths(self.dir, WIN), ops=ops,
                       tvram=tvram or (lambda: None),
                       status_fn=status_fn or (lambda: '{"phase":"p2"}'),
                       sleep=self.fc.sleep, clock=self.fc.clock,
                       out=self.out, err=self.err, poll=1.0, settle=2.0)

    def run_main(self, c, argv):
        return ctl.main(argv, ctl_factory=lambda: c)


class IniParse(Base):
    def test_reads_media_keys_only_and_leaves_ini_untouched(self):
        path = os.path.join(self.dir, 'test.ini')
        before = Path(path).read_bytes()
        got = ctl.ini_media(path)
        self.assertEqual(got, ['C:\\NP21\\os32_boot.d88', 'C:\\NP21\\os32.nhd',
                               'C:\\NP21\\os32_install.iso'])
        self.assertEqual(Path(path).read_bytes(), before)

    def test_unreadable_ini_is_none(self):
        self.assertIsNone(ctl.ini_media(os.path.join(self.dir, 'nope.ini')))


class LockWait(Base):
    def test_waits_until_lock_is_released_then_starts(self):
        nhd = WIN + '\\os32.nhd'
        c = self.make(FakeOps(locks={nhd: 3}))
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--fd', 'os32_boot.d88'])
        self.assertEqual(rc, 0, self.err.getvalue())
        # t=0..2 locked、t=3 から free。続けて 5 秒 (t=8) で通す。
        # 他の 2 つは t=5 で安定したので以後は聞き直さない
        self.assertEqual(len(self.ops.probes), 9)
        self.assertEqual(self.ops.probes[-1], [nhd])
        self.assertEqual(len(self.ops.probes[5]), 3)
        self.assertEqual(self.ops.probes[6], [nhd])
        self.assertEqual(self.ops.started, [(
            WIN + '\\np21x64w.exe',
            ['/i' + WIN + '\\test.ini', WIN + '\\os32_boot.d88'], WIN)])
        self.assertIn('started', self.out.getvalue())
        # FD は ini と --fd の両方にあるが 1 回だけ数える
        self.assertEqual(len(self.ops.probes[0]), 3)

    def test_open_once_then_locked_again_restarts_the_count(self):
        # nhd-pull の直後: 1 回開けてもすぐまた掴まれる (Defender の走査など)
        nhd = WIN + '\\os32.nhd'
        c = self.make(FakeOps(lock_seq={nhd: 'FLLF'}))
        rc = self.run_main(c, ['start', '--ini', 'test.ini'])
        self.assertEqual(rc, 0, self.err.getvalue())
        self.assertIn('locked again', self.out.getvalue())
        # t=0 free → t=1,2 locked → t=3 から free。t=0 の 1 回では通さず、t=8 まで待つ
        nhd_probes = [i for i, ps in enumerate(self.ops.probes) if nhd in ps]
        self.assertEqual(nhd_probes, list(range(9)))
        self.assertEqual(len(self.ops.started), 1)

    def test_flapping_lock_times_out_as_unsteady(self):
        nhd = WIN + '\\os32.nhd'
        c = self.make(FakeOps(lock_seq={nhd: 'FLFFFLFFFFLF' * 10}))
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--timeout', '20'])
        self.assertEqual(rc, 1)
        self.assertIn(nhd, self.err.getvalue())
        self.assertEqual(self.ops.started, [])

    def test_stable_zero_passes_on_first_free(self):
        c = self.make(FakeOps())
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--stable', '0',
                               '--alive', '0'])
        self.assertEqual(rc, 0, self.err.getvalue())
        self.assertEqual(len(self.ops.probes), 1)

    def test_timeout_names_locked_file_and_does_not_start(self):
        iso = WIN + '\\os32_install.iso'
        c = self.make(FakeOps(locks={iso: 10 ** 6}))
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--timeout', '5'])
        self.assertEqual(rc, 1)
        self.assertIn('os32_install.iso', self.err.getvalue())
        self.assertNotIn('os32.nhd', self.err.getvalue())
        self.assertEqual(self.ops.started, [])
        self.assertGreaterEqual(self.fc.now, 5)

    def test_missing_fd_argument_fails(self):
        c = self.make(FakeOps(missing={WIN + '\\nofd.d88'}))
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--fd', 'nofd.d88'])
        self.assertEqual(rc, 2)
        self.assertIn('nofd.d88', self.err.getvalue())
        self.assertEqual(self.ops.started, [])

    def test_missing_ini_media_is_warned_not_waited(self):
        iso = WIN + '\\os32_install.iso'
        c = self.make(FakeOps(missing={iso}))
        rc = self.run_main(c, ['start', '--ini', 'test.ini'])
        self.assertEqual(rc, 0)
        self.assertIn(iso, self.err.getvalue())
        self.assertEqual(len(self.ops.started), 1)


class ProcessRemains(Base):
    def test_start_refuses_while_np21_process_remains(self):
        c = self.make(FakeOps(procs=[[('np21x64w.exe', 77)]]))
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--timeout', '3'])
        self.assertEqual(rc, 1)
        self.assertIn('pid=77', self.err.getvalue())
        self.assertEqual(self.ops.probes, [])
        self.assertEqual(self.ops.started, [])

    def test_start_waits_for_process_to_disappear(self):
        c = self.make(FakeOps(procs=[[('np21x64w.exe', 77)], [('np21x64w.exe', 77)], []]))
        rc = self.run_main(c, ['start', '--ini', 'test.ini'])
        self.assertEqual(rc, 0, self.err.getvalue())
        self.assertEqual(len(self.ops.started), 1)

    def test_stop_kills_and_fails_when_process_survives(self):
        c = self.make(FakeOps(procs=[[('np21x64w.exe', 5), ('np21w.exe', 6)]]))
        rc = self.run_main(c, ['stop', '--timeout', '4'])
        self.assertEqual(rc, 1)
        self.assertEqual(self.ops.killed, [5, 6])
        self.assertIn('残っている', self.err.getvalue())

    def test_stop_succeeds_when_process_goes_away(self):
        c = self.make(FakeOps(procs=[[('np21x64w.exe', 5)], [('np21x64w.exe', 5)], []]))
        rc = self.run_main(c, ['stop'])
        self.assertEqual(rc, 0)
        self.assertEqual(self.ops.killed, [5])
        self.assertIn('stopped', self.out.getvalue())


class AfterStart(Base):
    def test_process_exits_right_after_start(self):
        c = self.make(FakeOps(dies_after=2))
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--wait-ready'])
        self.assertEqual(rc, 1)
        self.assertIn('起動直後に終了した', self.err.getvalue())
        self.assertIn('pid=4242', self.err.getvalue())
        self.assertEqual(len(self.ops.started), 1)

    def test_alive_for_the_whole_window_and_api_up(self):
        c = self.make(FakeOps())
        rc = self.run_main(c, ['start', '--ini', 'test.ini'])
        self.assertEqual(rc, 0, self.err.getvalue())
        self.assertIn('alive 10s pid=4242, aidebug up', self.out.getvalue())
        # 起動後に少なくとも 10 回 (10 秒) プロセスを見た
        self.assertGreaterEqual(self.ops.after_start, 11)

    def test_process_lives_but_api_never_answers(self):
        c = self.make(FakeOps(), status_fn=lambda: None)
        rc = self.run_main(c, ['start', '--ini', 'test.ini', '--api-timeout', '15'])
        self.assertEqual(rc, 1)
        self.assertIn('/api/status', self.err.getvalue())

    def test_api_comes_up_late(self):
        answers = [None] * 20 + ['{"phase":"p2"}']
        c = self.make(FakeOps(), status_fn=lambda: answers.pop(0) if answers else 'x')
        rc = self.run_main(c, ['start', '--ini', 'test.ini'])
        self.assertEqual(rc, 0, self.err.getvalue())


class IniUnreadable(Base):
    def test_falls_back_to_default_three_media(self):
        os.chmod(os.path.join(self.dir, 'test.ini'), 0)
        if os.access(os.path.join(self.dir, 'test.ini'), os.R_OK):
            self.skipTest('running as a user that ignores file modes')
        c = self.make(FakeOps())
        rc = self.run_main(c, ['start', '--ini', 'test.ini'])
        self.assertEqual(rc, 0, self.err.getvalue())
        self.assertEqual(sorted(self.ops.probes[0]),
                         sorted(WIN + '\\' + n for n in ctl.DEFAULT_MEDIA))
        self.assertIn('読めない', self.err.getvalue())

    def test_status_without_ini_uses_defaults(self):
        c = self.make(FakeOps(), status_fn=lambda: None)
        self.assertEqual(self.run_main(c, ['status']), 0)
        self.assertEqual(sorted(self.ops.probes[0]),
                         sorted(WIN + '\\' + n for n in ctl.DEFAULT_MEDIA))
        self.assertIn('process: none', self.out.getvalue())
        self.assertIn('aidebug: down', self.out.getvalue())


class Arguments(Base):
    def test_paths_are_rejected(self):
        for bad in ('../x.ini', 'C:\\x.ini', 'a/b.ini', '.hidden'):
            c = self.make(FakeOps())
            self.assertEqual(self.run_main(c, ['start', '--ini', bad]), 2, bad)
            self.assertEqual(self.ops.started, [])

    def test_missing_ini_file_is_argument_error(self):
        c = self.make(FakeOps())
        self.assertEqual(self.run_main(c, ['start', '--ini', 'none.ini']), 2)


class WaitReady(Base):
    def test_ready_when_text_appears(self):
        screens = [None, 'booting', 'OS32\nWaiting for commands via serial...\n']
        c = self.make(FakeOps(), tvram=lambda: screens.pop(0))
        self.assertEqual(self.run_main(c, ['wait-ready']), 0)
        self.assertIn('ready', self.out.getvalue())

    def test_timeout_shows_last_screen(self):
        c = self.make(FakeOps(), tvram=lambda: 'root panic\n')
        self.assertEqual(self.run_main(c, ['wait-ready', '--timeout', '6']), 1)
        self.assertIn('root panic', self.err.getvalue())

    def test_timeout_when_api_never_answers(self):
        c = self.make(FakeOps(), tvram=lambda: None)
        self.assertEqual(self.run_main(c, ['wait-ready', '--timeout', '6']), 1)
        self.assertIn('aidebug', self.err.getvalue())

    def test_start_with_wait_ready(self):
        c = self.make(FakeOps(), tvram=lambda: 'Waiting for commands')
        self.assertEqual(self.run_main(c, ['start', '--ini', 'test.ini', '--wait-ready']), 0)


class RealOpsParsing(unittest.TestCase):
    """WinOps の出力解釈だけを見る (_run を差し替え、Windows は呼ばない)。"""

    def ops(self, rc, out):
        o = ctl.WinOps()
        o.calls = []

        def run(args, timeout=60):
            o.calls.append(args)
            return rc, out
        o._run = run
        return o

    def test_tasklist_csv(self):
        o = self.ops(0, '"np21x64w.exe","1234","Console","1","50,000 K"\n'
                        '"explorer.exe","9","Console","1","1 K"\n'
                        '"NP21W.EXE","55","Console","1","1 K"\n')
        self.assertEqual(o.list_processes(), [('np21x64w.exe', 1234), ('NP21W.EXE', 55)])

    def test_probe_parses_and_unreported_is_locked(self):
        # 結果は添字で返る。範囲外の添字・知らない状態語は捨てる
        o = self.ops(0, 'free\t0\nmissing\t1\nfree\t9\nweird\t2\n')
        got = o.probe(['C:\\a.nhd', 'C:\\b.iso', 'C:\\c.d88'])
        self.assertEqual(got, {'C:\\a.nhd': 'free', 'C:\\b.iso': 'missing',
                               'C:\\c.d88': 'locked'})
        self.assertIn('-EncodedCommand', o.calls[0])

    def test_start_returns_pid_or_fails(self):
        self.assertEqual(self.ops(0, 'pid\t321\n').start('C:\\e.exe', ['/iC:\\a.ini'], 'C:\\'), 321)
        with self.assertRaises(ctl.CtlError):
            self.ops(1, 'boom').start('C:\\e.exe', [], 'C:\\')


class Resolve(unittest.TestCase):
    def test_env_then_dotenv_then_sample(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, '.env.sample'), 'w') as f:
                f.write('NP21W_DIR=/sample\n')
            self.assertEqual(ctl.resolve_np21w_dir({}, d), '/sample')
            with open(os.path.join(d, '.env'), 'w') as f:
                f.write('X=1\nNP21W_DIR=/mnt/c/np\n')
            self.assertEqual(ctl.resolve_np21w_dir({}, d), '/mnt/c/np')
            self.assertEqual(ctl.resolve_np21w_dir({'NP21W_DIR': '/e'}, d), '/e')

    def test_win_path(self):
        self.assertEqual(ctl.to_win_path('/mnt/c/Users/a/np21w'), 'C:\\Users\\a\\np21w')
        p = ctl.Paths('/mnt/c/np/', None)
        self.assertEqual(p.win_of('a.ini'), 'C:\\np\\a.ini')
        self.assertEqual(ctl.Paths('/mnt/c/np', 'D:\\x\\').win_of('a'), 'D:\\x\\a')

    def test_powershell_quoting(self):
        self.assertEqual(ctl._ps_quote("C:\\it's"), "'C:\\it''s'")


if __name__ == '__main__':
    unittest.main()
