"""S0-D: 通常配備が /etc/settings.db* を作らない・上書きしない・消さないことの試験。

票:   docs/tasks/settings/TASK_S0.md §2 (S0-D)、契約は S0_FOUNDATION.md §5 (D0)
記録: tools/tests/s0_tdd.md 節 D

設定レジストリ (/etc/settings.db) は**ゲストが書く**もので、ホストのビルド成果物では
ない。配備ツールは「マニフェストにある物を書く」「HostDrv の中身をそのまま NHD へ
写す」「マニフェストに無い物を消す」「NHD イメージを丸ごと上書きする」の 4 系統を
持ち、どれか 1 つでも settings.db を掴むとユーザーの設定が消える。

temp dir と mock だけで走る。sudo / mount / losetup / mkfs / 実配備は FakeRun が
**例外にして遮断**する (呼ばれたら試験が落ちる)。エミュレータにも make にも触れない。
"""
import errno
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

# import 時に .env / .env.sample を読ませない。実在しない場所を渡しておき、
# 各 TestCase が temp dir を指し直す。
_SESSION = tempfile.TemporaryDirectory(prefix='os32-protect-session-')
os.environ['NP21W_DIR'] = os.path.join(_SESSION.name, 'np21w')
os.environ['HOSTDRV_DIR'] = os.path.join(_SESSION.name, 'hostdrv')
os.environ['OS32_NHD_LOCAL'] = os.path.join(_SESSION.name, 'os32.nhd')

sys.path.insert(0, str(ROOT / 'tools'))
import deploy_protect as protect          # noqa: E402
import nhd_deploy as nd                   # noqa: E402
import hostdrv_deploy as hd               # noqa: E402
import prune_stale as ps                  # noqa: E402

PROTECTED = sorted(protect.PROTECTED_BASENAMES)
REAL_RUN = subprocess.run


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        h.update(f.read())
    return h.hexdigest()


def tree_state(root):
    """root 配下の「存在一覧 + 内容 hash」。配備の前後で比較する。"""
    state = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames:
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            state[rel + '/'] = 'dir'
        for name in filenames:
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root)
            state[rel] = sha256(p)
    return state


class R(object):
    def __init__(self, rc=0, stderr='', stdout=''):
        self.returncode = rc
        self.stderr = stderr
        self.stdout = stdout


class FakeRun(object):
    """subprocess.run の差し替え。実配備の操作は例外にして遮断する。"""

    BLOCKED = ('mount', 'umount', 'losetup', 'mkfs.ext2', 'mke2fs', 'e2fsck',
               'taskkill.exe')

    def __init__(self, sandbox=None):
        self.calls = []
        self.fail_cp = False
        self.fail_sync = False
        self.fail_rm = False
        self.fail_mkdir = False
        self.mounted = True
        # ここより外には 1 バイトも書かない (隔離の保証)
        self.sandbox = os.path.realpath(sandbox) if sandbox else None

    def _in_sandbox(self, path):
        if self.sandbox is None:
            return True
        real = os.path.realpath(path)
        return real == self.sandbox or real.startswith(self.sandbox + os.sep)

    def _check(self, path):
        if not self._in_sandbox(path):
            raise AssertionError('temp の外を触ろうとした: %r' % (path,))

    # --- 記録の問い合わせ -------------------------------------------------
    def write_targets(self):
        """書く / 消す系のコマンドが触った宛先パスの一覧。"""
        out = []
        for cmd in self.calls:
            argv = cmd[1:] if cmd and cmd[0] == 'sudo' else list(cmd)
            if not argv:
                continue
            if argv[0] == 'cp':
                dst = argv[-1]
                if os.path.isdir(dst):
                    dst = os.path.join(dst, os.path.basename(argv[-2]))
                out.append(dst)
            elif argv[0] in ('rm', 'mkdir'):
                out.extend(a for a in argv[1:] if not a.startswith('-'))
        return out

    def touched_protected(self):
        hits = []
        for p in self.write_targets():
            if os.path.basename(p).lower() in protect.PROTECTED_BASENAMES:
                hits.append(p)
        return hits

    # --- 実行 -------------------------------------------------------------
    def __call__(self, cmd, *args, **kwargs):
        if not isinstance(cmd, (list, tuple)):
            raise AssertionError('shell 文字列の実行は禁止: %r' % (cmd,))
        self.calls.append(list(cmd))
        argv = list(cmd)
        if argv[0] == 'sudo':
            argv = argv[1:]
        prog = os.path.basename(argv[0])

        if prog in self.BLOCKED:
            raise AssertionError('実配備の操作が呼ばれた: %r' % (cmd,))
        if prog == 'mountpoint':
            return R(0 if self.mounted else 1)
        if prog == 'sync':
            return R(1, 'sync: I/O error') if self.fail_sync else R(0)
        if prog == 'mkdir':
            if self.fail_mkdir:
                return R(1, 'mkdir: No space left on device')
            for p in argv[1:]:
                if not p.startswith('-'):
                    self._check(p)
                    os.makedirs(p, exist_ok=True)
            return R(0)
        if prog == 'cp':
            src, dst = argv[-2], argv[-1]
            # 実物の cp と同じ: 宛先が既存ディレクトリなら**中へ**書く
            if os.path.isdir(dst):
                dst = os.path.join(dst, os.path.basename(src))
            self._check(dst)
            if self.fail_cp:
                with open(dst, 'wb') as f:      # cp は書く前に切り詰める
                    f.write(b'PARTIAL')
                return R(1, "cp: error writing '%s': No space left" % dst)
            shutil.copyfile(src, dst)
            return R(0)
        if prog == 'rm':
            if self.fail_rm:
                return R(1, 'rm: cannot remove')
            for p in argv[1:]:
                if p.startswith('-'):
                    continue
                self._check(p)
                try:
                    os.remove(p)
                except OSError as exc:
                    if exc.errno != errno.ENOENT:
                        return R(1, str(exc))
            return R(0)
        if prog == 'ls':
            return R(0, stdout='')
        raise AssertionError('想定外のコマンド: %r' % (cmd,))


class Base(unittest.TestCase):
    """temp dir に NHD マウント先 / HostDrv / ビルド成果物を作る。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='os32-protect-')
        self.root = pathlib.Path(os.path.realpath(self.tmp.name))
        self.mount = self.root / 'mnt'
        self.hostdrv = self.root / 'hostdrv'
        self.build = self.root / 'build'
        for d in (self.mount / 'etc', self.mount / 'bin',
                  self.hostdrv / 'etc', self.build):
            d.mkdir(parents=True)

        # ゲストが書いた本物の設定 DB (これが不変であることが合否)
        self.db = self.mount / 'etc' / 'settings.db'
        self.db.write_bytes(b'GUEST-SETTINGS-DB' * 64)
        self.journal = self.mount / 'etc' / 'settings.db-journal'
        self.journal.write_bytes(b'HOT-JOURNAL')

        # ホスト側の「配備しようとしてしまう」ソース (意図的な fixture)
        self.src_db = self.build / 'settings.db'
        self.src_db.write_bytes(b'HOST-BUILT-DB-DIFFERENT')
        self.src_journal = self.build / 'settings.db-journal'
        self.src_journal.write_bytes(b'HOST-JOURNAL')
        self.src_tsv = self.build / 'defaults.tsv'
        self.src_tsv.write_bytes(b'gshell\tdesktop/color\tint\t1\n')
        self.src_bin = self.build / 'sh.bin'
        self.src_bin.write_bytes(b'BINARY' * 100)

        self.fake = FakeRun(sandbox=str(self.root))
        self._saved = {}
        self._patch(subprocess, 'run', self.fake)
        self._patch(nd, 'MOUNT_POINT', str(self.mount))
        self._patch(nd, 'NHD_LOCAL', str(self.root / 'os32.nhd'))
        self._patch(nd, 'NHD_REMOTE', str(self.root / 'remote' / 'os32.nhd'))
        self._patch(hd, 'HOSTDRV_DIR', str(self.hostdrv))
        os.environ['HOSTDRV_DIR'] = str(self.hostdrv)

    def tearDown(self):
        for (obj, name), value in self._saved.items():
            setattr(obj, name, value)
        self.tmp.cleanup()

    def _patch(self, obj, name, value):
        key = (obj, name)
        if key not in self._saved:
            self._saved[key] = getattr(obj, name)
        setattr(obj, name, value)

    # --- 共通のアサーション ----------------------------------------------
    def assertNoProtectedWrites(self):
        hits = self.fake.touched_protected()
        self.assertEqual(hits, [], '保護対象に write 系が走った: %r' % (hits,))

    def assertDbIntact(self, digest):
        self.assertTrue(self.db.exists(), '/etc/settings.db が消えた')
        self.assertEqual(sha256(self.db), digest,
                         '/etc/settings.db の内容が変わった')

    def manifest(self, files, directories=()):
        cfg = {'boot': {},
               'filesystem': {'directories': list(directories),
                              'files': list(files)}}
        self._patch(nd, 'load_deploy_yaml', lambda: cfg)
        self._patch(hd, 'load_deploy_yaml', lambda: cfg)
        return cfg

    def pairs(self, mapping):
        """resolve_files_from_entry を差し替えて (host, guest) を直接与える。"""
        def resolve(entry):
            return list(mapping.get(entry['host'], []))
        self._patch(nd, 'resolve_files_from_entry', resolve)
        self._patch(hd, 'resolve_files_from_entry', resolve)


# ======================================================================
#  (3) 判定そのもの — 名前規則 / 実体規則 / 迂回
# ======================================================================
class ProtectJudgement(Base):

    def test_name_rule_matches_protected_basenames(self):
        for name in PROTECTED:
            dest = protect.resolve_dest(str(self.mount), '/etc/' + name)
            self.assertTrue(protect.is_protected(str(self.mount), dest), name)

    def test_directory_form_guest_path(self):
        """`guest: /etc/` はソースの basename を補って最終パスを決める。"""
        dest = protect.resolve_dest(str(self.mount), '/etc/',
                                    str(self.src_db))
        self.assertEqual(dest, str(self.mount / 'etc' / 'settings.db'))
        self.assertTrue(protect.is_protected(str(self.mount), dest))

    def test_dotdot_and_redundant_slashes(self):
        dest = protect.resolve_dest(str(self.mount), '/bin/..//etc/./settings.db')
        self.assertEqual(dest, str(self.mount / 'etc' / 'settings.db'))
        self.assertTrue(protect.is_protected(str(self.mount), dest))

    def test_uppercase_basename(self):
        dest = protect.resolve_dest(str(self.mount), '/etc/SETTINGS.DB')
        self.assertTrue(protect.is_protected(str(self.mount), dest))

    def test_escape_root_is_refused(self):
        with self.assertRaises(protect.ProtectError):
            protect.resolve_dest(str(self.mount), '/../../etc/settings.db')

    def test_name_rule_applies_before_realpath(self):
        """実体が欠損していても名前で守る (欠損は欠損のまま)。"""
        self.db.unlink()
        self.journal.unlink()
        dest = protect.resolve_dest(str(self.mount), '/etc/settings.db')
        self.assertTrue(protect.is_protected(str(self.mount), dest))

    def test_dangling_symlink_alias(self):
        """etc/settings.db を指す dangling symlink 経由でも作らせない。"""
        self.db.unlink()
        alias = self.mount / 'bin' / 'alias.bin'
        os.symlink(str(self.db), str(alias))
        self.assertTrue(protect.is_protected(str(self.mount), str(alias)))

    def test_symlink_alias_to_live_db(self):
        alias = self.mount / 'bin' / 'alias.bin'
        os.symlink(str(self.db), str(alias))
        self.assertTrue(protect.is_protected(str(self.mount), str(alias)))

    def test_hardlink_alias(self):
        alias = self.mount / 'bin' / 'hard.bin'
        os.link(str(self.db), str(alias))
        self.assertTrue(protect.is_protected(str(self.mount), str(alias)),
                        'hardlink の別名を実体規則が見逃した')

    def test_enoent_is_not_protected(self):
        """確定した不存在だけ「保護対象ではない」= 初回の tsv を配備できる。"""
        dest = protect.resolve_dest(str(self.mount), '/etc/settings.tsv')
        self.assertFalse(protect.is_protected(str(self.mount), dest))

    def test_unreadable_stat_fails_the_deploy(self):
        if os.geteuid() == 0:
            self.skipTest('root では EACCES を作れない')
        locked = self.mount / 'locked'
        locked.mkdir()
        (locked / 'x.bin').write_bytes(b'x')
        os.chmod(str(locked), 0o000)
        try:
            with self.assertRaises(protect.ProtectError):
                protect.is_protected(str(self.mount), str(locked / 'x.bin'))
        finally:
            os.chmod(str(locked), 0o755)

    def test_etc_symlink_refuses_whole_deploy(self):
        alt = self.root / 'mnt2'
        (alt / 'conf').mkdir(parents=True)
        os.symlink(str(alt / 'conf'), str(alt / 'etc'))
        with self.assertRaises(protect.ProtectError):
            protect.check_root_etc(str(alt))
        with self.assertRaises(protect.ProtectError):
            protect.is_protected(str(alt), str(alt / 'bin.bin'))

    def test_normal_files_pass(self):
        for guest in ('/bin/sh.bin', '/etc/settings.tsv', '/etc/motd',
                      '/etc/sub/settings.db', '/settings.db'):
            dest = protect.resolve_dest(str(self.mount), guest)
            self.assertFalse(protect.is_protected(str(self.mount), dest), guest)


# ======================================================================
#  (1)(2) NHD の manifest 配備
# ======================================================================
class NhdSync(Base):

    def setUp(self):
        super(NhdSync, self).setUp()
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)

    def _run_sync(self, files, directories=(), mapping=None):
        self.manifest(files, directories)
        self.pairs(mapping or {})
        return nd.do_sync()

    def test_protected_source_is_skipped_and_db_unchanged(self):
        before = tree_state(str(self.mount))
        digest = sha256(self.db)
        ok = self._run_sync(
            [{'host': 'build/settings.db', 'guest': '/etc/settings.db',
              'tags': ['core']},
             {'host': 'build/settings.db-journal',
              'guest': '/etc/settings.db-journal', 'tags': ['core']},
             {'host': 'build/defaults.tsv', 'guest': '/etc/settings.tsv',
              'tags': ['core']}],
            mapping={
                'build/settings.db': [(str(self.src_db), '/etc/settings.db')],
                'build/settings.db-journal': [
                    (str(self.src_journal), '/etc/settings.db-journal')],
                'build/defaults.tsv': [
                    (str(self.src_tsv), '/etc/settings.tsv')],
            })
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertEqual(self.journal.read_bytes(), b'HOT-JOURNAL')
        self.assertNoProtectedWrites()
        # tsv は通常配備で更新される
        tsv = self.mount / 'etc' / 'settings.tsv'
        self.assertTrue(tsv.exists())
        self.assertEqual(tsv.read_bytes(), self.src_tsv.read_bytes())
        after = tree_state(str(self.mount))
        after.pop('etc/settings.tsv')
        before.pop('etc/settings.tsv', None)
        self.assertEqual(before, after, '保護対象以外の既存物まで変わった')

    def test_missing_db_stays_missing(self):
        self.db.unlink()
        self.journal.unlink()
        ok = self._run_sync(
            [{'host': 'build/settings.db', 'guest': '/etc/settings.db',
              'tags': ['core']}],
            mapping={'build/settings.db': [(str(self.src_db),
                                            '/etc/settings.db')]})
        self.assertIs(ok, True)
        self.assertFalse(self.db.exists(), '通常配備が DB を作ってしまった')
        self.assertNoProtectedWrites()

    def test_directory_style_guest_path(self):
        digest = sha256(self.db)
        ok = self._run_sync(
            [{'host': 'build/*.db', 'guest': '/etc/', 'type': 'glob',
              'tags': ['core']}],
            mapping={'build/*.db': [(str(self.src_db), '/etc/settings.db')]})
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_dotdot_detour_is_blocked(self):
        digest = sha256(self.db)
        ok = self._run_sync(
            [{'host': 'build/settings.db', 'guest': '/bin/../etc/settings.db',
              'tags': ['core']}],
            mapping={'build/settings.db': [
                (str(self.src_db), '/bin/../etc/settings.db')]})
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_protected_directory_is_not_created(self):
        """etc/settings.db/ という残骸ディレクトリを作らない。"""
        ok = self._run_sync([], directories=['/etc/settings.db', '/opt'])
        self.assertIs(ok, True)
        self.assertTrue(self.db.is_file(), 'DB がディレクトリに化けた')
        self.assertTrue((self.mount / 'opt').is_dir())
        self.assertNoProtectedWrites()

    def test_normal_deploy_has_no_protected_line(self):
        import io
        buf = io.StringIO()
        saved = sys.stdout
        sys.stdout = buf
        try:
            ok = self._run_sync(
                [{'host': 'build/sh.bin', 'guest': '/bin/sh.bin',
                  'tags': ['core']}],
                mapping={'build/sh.bin': [(str(self.src_bin), '/bin/sh.bin')]})
        finally:
            sys.stdout = saved
        self.assertIs(ok, True)
        self.assertNotIn('protected:', buf.getvalue())
        self.assertEqual((self.mount / 'bin' / 'sh.bin').read_bytes(),
                         self.src_bin.read_bytes())

    def test_cp_failure_is_not_success(self):
        self.fake.fail_cp = True
        ok = self._run_sync(
            [{'host': 'build/sh.bin', 'guest': '/bin/sh.bin', 'tags': ['core']}],
            mapping={'build/sh.bin': [(str(self.src_bin), '/bin/sh.bin')]})
        self.assertIs(ok, False)
        self.assertFalse((self.mount / 'bin' / 'sh.bin').exists(),
                         '切り詰められた宛先が残っている')

    def test_sync_failure_is_not_success(self):
        self.fake.fail_sync = True
        ok = self._run_sync(
            [{'host': 'build/sh.bin', 'guest': '/bin/sh.bin', 'tags': ['core']}],
            mapping={'build/sh.bin': [(str(self.src_bin), '/bin/sh.bin')]})
        self.assertIs(ok, False)


# ======================================================================
#  (3) HostDrv -> NHD の再帰コピー
# ======================================================================
class SyncFromHostdrv(Base):

    def test_stale_hostdrv_db_does_not_truncate_nhd_db(self):
        digest = sha256(self.db)
        (self.hostdrv / 'etc' / 'settings.db').write_bytes(b'STALE')
        (self.hostdrv / 'etc' / 'settings.tsv').write_bytes(b'k\tv\n')
        (self.hostdrv / 'bin').mkdir()
        (self.hostdrv / 'bin' / 'sh.bin').write_bytes(b'NEWBIN')
        ok = nd.do_sync_from_hostdrv()
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()
        self.assertEqual((self.mount / 'bin' / 'sh.bin').read_bytes(), b'NEWBIN')
        self.assertEqual((self.mount / 'etc' / 'settings.tsv').read_bytes(),
                         b'k\tv\n')

    def test_protected_directory_in_hostdrv_is_not_created(self):
        (self.hostdrv / 'etc' / 'settings.db-wal').mkdir()
        (self.hostdrv / 'etc' / 'settings.db-wal' / 'junk').write_bytes(b'x')
        ok = nd.do_sync_from_hostdrv()
        self.assertIs(ok, True)
        self.assertFalse((self.mount / 'etc' / 'settings.db-wal').exists())
        self.assertNoProtectedWrites()

    def test_sync_failure_is_not_success(self):
        (self.hostdrv / 'bin').mkdir()
        (self.hostdrv / 'bin' / 'sh.bin').write_bytes(b'NEWBIN')
        self.fake.fail_sync = True
        self.assertIs(nd.do_sync_from_hostdrv(), False)


# ======================================================================
#  (3)(4) copy / copy-all / rm の CLI
# ======================================================================
class NhdCli(Base):

    def test_copy_cannot_write_protected(self):
        digest = sha256(self.db)
        ok = nd.do_copy([str(self.src_db)], dest_dir='/etc')
        self.assertIs(ok, True)                 # 除外は失敗ではない
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_copy_rename_cannot_write_protected(self):
        digest = sha256(self.db)
        ok = nd.do_copy([str(self.src_bin)], dest_dir='/etc',
                        rename='settings.db')
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_copy_all_skips_protected_and_copies_rest(self):
        digest = sha256(self.db)
        batch = self.root / 'batch'
        batch.mkdir()
        (batch / 'settings.db').write_bytes(b'HOST')
        (batch / 'motd').write_bytes(b'hello')
        ok = nd.do_copy_all(str(batch), ext='', dest_dir='/etc')
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertEqual((self.mount / 'etc' / 'motd').read_bytes(), b'hello')
        self.assertNoProtectedWrites()

    def test_copy_failure_is_not_success(self):
        self.fake.fail_cp = True
        self.assertIs(nd.do_copy([str(self.src_bin)], dest_dir='/bin'), False)

    def test_rm_cannot_remove_protected(self):
        digest = sha256(self.db)
        ok = nd.do_rm('/etc/settings.db')
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_rm_failure_is_not_success(self):
        (self.mount / 'bin' / 'old.bin').write_bytes(b'x')
        self.fake.fail_rm = True
        self.assertIs(nd.do_rm('/bin/old.bin'), False)

    def test_rm_success_returns_true(self):
        (self.mount / 'bin' / 'old.bin').write_bytes(b'x')
        self.assertIs(nd.do_rm('/bin/old.bin'), True)
        self.assertFalse((self.mount / 'bin' / 'old.bin').exists())


# ======================================================================
#  (1)(3) HostDrv 配備と clean
# ======================================================================
class HostdrvSync(Base):

    def test_protected_is_judged_before_content_comparison(self):
        stale = self.hostdrv / 'etc' / 'settings.db'
        stale.write_bytes(b'OLD-HOSTDRV-DB')
        digest = sha256(stale)

        import filecmp
        opened = []

        def watched(a, b, shallow=True):
            opened.append((a, b))
            return filecmp.cmp(a, b, shallow=shallow)
        self._patch(hd.filecmp, 'cmp', watched)

        self.manifest([
            {'host': 'build/settings.db', 'guest': '/etc/settings.db',
             'tags': ['core']},
            {'host': 'build/defaults.tsv', 'guest': '/etc/settings.tsv',
             'tags': ['core']}])
        self.pairs({
            'build/settings.db': [(str(self.src_db), '/etc/settings.db')],
            'build/defaults.tsv': [(str(self.src_tsv), '/etc/settings.tsv')]})

        self.assertIs(hd.do_sync(), True)
        self.assertEqual(sha256(stale), digest, 'HostDrv の DB が上書きされた')
        for a, b in opened:
            self.assertNotIn('settings.db', os.path.basename(b),
                             '保護対象を内容比較のために開いている')
        self.assertEqual((self.hostdrv / 'etc' / 'settings.tsv').read_bytes(),
                         self.src_tsv.read_bytes())

    def test_clean_keeps_protected_and_its_ancestors(self):
        (self.hostdrv / 'etc' / 'settings.db').write_bytes(b'KEEP-ME')
        (self.hostdrv / 'etc' / 'settings.tsv').write_bytes(b'drop')
        (self.hostdrv / 'bin').mkdir()
        (self.hostdrv / 'bin' / 'sh.bin').write_bytes(b'drop')
        (self.hostdrv / 'top.bin').write_bytes(b'drop')

        self.assertIs(hd.do_clean(), True)
        self.assertTrue((self.hostdrv / 'etc' / 'settings.db').exists(),
                        'clean が保護対象を消した')
        self.assertEqual((self.hostdrv / 'etc' / 'settings.db').read_bytes(),
                         b'KEEP-ME')
        self.assertTrue((self.hostdrv / 'etc').is_dir(), '祖先が消えた')
        self.assertFalse((self.hostdrv / 'etc' / 'settings.tsv').exists())
        self.assertFalse((self.hostdrv / 'bin').exists())
        self.assertFalse((self.hostdrv / 'top.bin').exists())

    def test_clean_without_protected_empties_everything(self):
        (self.hostdrv / 'bin').mkdir()
        (self.hostdrv / 'bin' / 'sh.bin').write_bytes(b'drop')
        self.assertIs(hd.do_clean(), True)
        # 保護対象が無ければ従来どおり空になる (空の etc も残さない)
        self.assertEqual(os.listdir(str(self.hostdrv)), [])


# ======================================================================
#  (3)(4) prune
# ======================================================================
class Prune(Base):

    def setUp(self):
        super(Prune, self).setUp()
        # NHD の取り込み・マウントは試験の対象外 (FakeRun が実操作を遮断する)
        self._patch(nd, 'ensure_local_nhd', lambda: True)

    def _stale(self, root, names):
        entries = []
        for guest, rel in names:
            p = os.path.join(root, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            if not os.path.exists(p):
                with open(p, 'wb') as f:
                    f.write(b'stale')
            entries.append((guest, p))
        self._patch(ps, 'find_stale', lambda r, w: list(entries))
        return entries

    def test_prune_hostdrv_keeps_protected(self):
        (self.hostdrv / 'etc' / 'settings.db').write_bytes(b'KEEP')
        self._stale(str(self.hostdrv),
                    [('/etc/settings.db', 'etc/settings.db'),
                     ('/bin/old.bin', 'bin/old.bin')])
        self._patch(ps, 'hostdrv_root', lambda: str(self.hostdrv))
        # 件数は**実際に消した数**。保護除外は数えない。
        n = ps.prune_hostdrv(set(), True)
        self.assertEqual(n, 1)
        self.assertTrue((self.hostdrv / 'etc' / 'settings.db').exists(),
                        'prune が保護対象を消した')
        self.assertFalse((self.hostdrv / 'bin' / 'old.bin').exists())

    def test_prune_nhd_keeps_protected(self):
        digest = sha256(self.db)
        self._stale(str(self.mount),
                    [('/etc/settings.db', 'etc/settings.db'),
                     ('/bin/old.bin', 'bin/old.bin')])
        n = ps.prune_nhd(set(), True)
        self.assertEqual(n, 1)
        self.assertDbIntact(digest)
        self.assertFalse((self.mount / 'bin' / 'old.bin').exists())
        self.assertNoProtectedWrites()

    def test_prune_nhd_rm_failure_is_error(self):
        self._stale(str(self.mount), [('/bin/old.bin', 'bin/old.bin')])
        self.fake.fail_rm = True
        self.assertIsNone(ps.prune_nhd(set(), True))

    def test_prune_nhd_sync_failure_is_error(self):
        self._stale(str(self.mount), [('/bin/old.bin', 'bin/old.bin')])
        self.fake.fail_sync = True
        self.assertIsNone(ps.prune_nhd(set(), True))

    def test_prune_hostdrv_rm_failure_is_error(self):
        self._stale(str(self.hostdrv), [('/bin/old.bin', 'bin/old.bin')])
        self._patch(ps, 'hostdrv_root', lambda: str(self.hostdrv))

        def boom(path):
            raise OSError(errno.EACCES, 'denied')
        self._patch(os, 'remove', boom)
        self.assertIsNone(ps.prune_hostdrv(set(), True))


# ======================================================================
#  (5) NHD イメージ全体の配備 — pull の来歴 (stamp)
# ======================================================================
class Stamp(Base):

    def setUp(self):
        super(Stamp, self).setUp()
        self.fake.mounted = False
        self.local = pathlib.Path(nd.NHD_LOCAL)
        self.remote = pathlib.Path(nd.NHD_REMOTE)
        self.remote.parent.mkdir(parents=True, exist_ok=True)
        self.remote.write_bytes(b'REMOTE-IMAGE-CONTENT' * 16)
        self._patch(nd, 'do_mount', lambda: True)

    def _pull(self):
        return nd.do_pull()

    def test_pull_writes_stamp_and_deploy_passes(self):
        self.assertIs(self._pull(), True)
        self.assertTrue(os.path.isfile(nd.stamp_path()))
        self.assertIs(nd.do_deploy(), True)
        self.assertEqual(self.remote.read_bytes(), self.local.read_bytes())

    def test_deploy_without_stamp_fails(self):
        self.assertIs(self._pull(), True)
        os.remove(nd.stamp_path())
        self.assertIs(nd.do_deploy(), False)

    def test_deploy_with_foreign_local_path_fails(self):
        self.assertIs(self._pull(), True)
        other = self.root / 'other.nhd'
        shutil.copy2(str(self.local), str(other))
        shutil.copy2(nd.stamp_path(), str(other) + nd.STAMP_SUFFIX)
        self._patch(nd, 'NHD_LOCAL', str(other))
        self.assertIs(nd.do_deploy(), False)

    def test_deploy_after_remote_changed_fails(self):
        """mtime を保ったまま中身だけ変わっても (ゲストが書いても) 検出する。"""
        self.assertIs(self._pull(), True)
        st = os.stat(str(self.remote))
        self.remote.write_bytes(b'GUEST-WROTE-SETTINGS' * 16)
        os.utime(str(self.remote), (st.st_atime, st.st_mtime))
        self.assertEqual(os.path.getsize(str(self.remote)), st.st_size)
        self.assertIs(nd.do_deploy(), False)

    def test_force_overrides(self):
        self.assertIs(self._pull(), True)
        os.remove(nd.stamp_path())
        self.assertIs(nd.do_deploy(force=True), True)

    def test_missing_local_auto_pulls_and_deploy_passes(self):
        """local を消しただけなら自動 pull が来歴を書いて deploy が通る。"""
        self.assertFalse(self.local.exists())
        self.assertIs(nd.do_deploy(), True)
        self.assertTrue(os.path.isfile(nd.stamp_path()))

    def test_failed_pull_leaves_no_stamp(self):
        self.assertIs(self._pull(), True)
        self.assertTrue(os.path.isfile(nd.stamp_path()))

        def boom(src, dst):
            raise OSError(errno.EACCES, 'locked by NP21/W')
        self._patch(nd.shutil, 'copy2', boom)
        self.assertIs(nd.do_pull(), False)
        self.assertFalse(os.path.isfile(nd.stamp_path()),
                         '失敗した pull が来歴を残した')
        self.assertIs(nd.do_deploy(), False)

    def test_deploy_refreshes_stamp(self):
        """書いた直後は remote == local。続けて deploy しても誤検出しない。"""
        self.assertIs(self._pull(), True)
        self.assertIs(nd.do_deploy(), True)
        self.assertIs(nd.do_deploy(), True)


# ======================================================================
#  Codex 実装レビュー 往復 1 の blocker B1〜B9 の反例
#  (docs/tasks/settings/TASK_S0.md §6 / tools/tests/s0_tdd.md §D)
# ======================================================================
class ReviewB1DirDestination(Base):
    """B1: cp / copy2 は宛先が既存ディレクトリなら中へ書く。"""

    def test_resolve_dest_fills_basename_for_existing_dir(self):
        dest = protect.resolve_dest(str(self.mount), '/etc', str(self.src_db))
        self.assertEqual(dest, str(self.mount / 'etc' / 'settings.db'))
        self.assertTrue(protect.is_protected(str(self.mount), dest))

    def test_copy_rename_into_root_cannot_reach_db(self):
        """`copy --dest / --rename etc <settings.db>` の反例。"""
        digest = sha256(self.db)
        src = self.root / 'settings.db'
        src.write_bytes(b'HOST-DB')
        ok = nd.do_copy([str(src)], dest_dir='/', rename='etc')
        self.assertIs(ok, True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_manifest_guest_without_slash_cannot_reach_db(self):
        """manifest の `guest: /etc` (末尾 '/' 無し) でも届かない。"""
        digest = sha256(self.db)
        self.manifest([{'host': 'build/settings.db', 'guest': '/etc',
                        'tags': ['core']}])
        self.pairs({'build/settings.db': [(str(self.src_db), '/etc')]})
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)
        self.assertIs(nd.do_sync(), True)
        self.assertDbIntact(digest)
        self.assertNoProtectedWrites()

    def test_hostdrv_guest_without_slash_cannot_reach_db(self):
        stale = self.hostdrv / 'etc' / 'settings.db'
        stale.write_bytes(b'OLD')
        digest = sha256(stale)
        self.manifest([{'host': 'build/settings.db', 'guest': '/etc',
                        'tags': ['core']}])
        self.pairs({'build/settings.db': [(str(self.src_db), '/etc')]})
        self.assertIs(hd.do_sync(), True)
        self.assertEqual(sha256(stale), digest)

    def test_cp_never_receives_a_directory(self):
        """cp / copy2 には確定したファイルパスだけを渡す。"""
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)
        self.manifest([{'host': 'build/sh.bin', 'guest': '/bin',
                        'tags': ['core']}])
        self.pairs({'build/sh.bin': [(str(self.src_bin), '/bin')]})
        self.assertIs(nd.do_sync(), True)
        for cmd in self.fake.calls:
            argv = cmd[1:] if cmd[0] == 'sudo' else cmd
            if argv and argv[0] == 'cp':
                self.assertFalse(os.path.isdir(argv[-1]),
                                 'cp にディレクトリを渡している: %r' % (cmd,))
        self.assertEqual((self.mount / 'bin' / 'sh.bin').read_bytes(),
                         self.src_bin.read_bytes())


class ReviewB2MkdirChain(Base):
    """B2: `mkdir -p` / makedirs は途中の祖先を黙って作る。"""

    def test_mkdir_chain_rejects_protected_ancestor(self):
        with self.assertRaises(protect.ProtectedPath):
            protect.mkdir_chain(str(self.mount), '/etc/settings.db/a')

    def test_mkdir_chain_returns_each_ancestor(self):
        chain = protect.mkdir_chain(str(self.mount), '/usr/share/doc')
        self.assertEqual(chain, [str(self.mount / 'usr'),
                                 str(self.mount / 'usr' / 'share'),
                                 str(self.mount / 'usr' / 'share' / 'doc')])

    def test_directories_entry_with_protected_ancestor(self):
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)
        self.manifest([], directories=['/etc/settings.db/a', '/opt'])
        self.pairs({})
        self.assertIs(nd.do_sync(), True)
        self.assertTrue(self.db.is_file(), 'DB がディレクトリに化けた')
        self.assertTrue((self.mount / 'opt').is_dir())
        self.assertNoProtectedWrites()

    def test_copy_dest_under_protected_ancestor(self):
        digest = sha256(self.db)
        ok = nd.do_copy([str(self.src_bin)], dest_dir='/etc/settings.db/a')
        self.assertIs(ok, True)                 # B7: 除外は成功
        self.assertDbIntact(digest)
        self.assertFalse((self.mount / 'etc' / 'settings.db' / 'a').exists())
        self.assertNoProtectedWrites()

    def test_hostdrv_directories_with_protected_ancestor(self):
        (self.hostdrv / 'etc' / 'settings.db').write_bytes(b'KEEP')
        digest = sha256(self.hostdrv / 'etc' / 'settings.db')
        self.manifest([], directories=['/etc/settings.db/a'])
        self.pairs({})
        self.assertIs(hd.do_sync(), True)
        self.assertTrue((self.hostdrv / 'etc' / 'settings.db').is_file())
        self.assertEqual(sha256(self.hostdrv / 'etc' / 'settings.db'), digest)


class ReviewB3Clean(Base):
    """B3: clean のディレクトリ処理に無判定削除があった。"""

    def test_symlink_named_protected_is_not_removed(self):
        target = self.root / 'elsewhere'
        target.mkdir()
        link = self.hostdrv / 'etc' / 'settings.db'
        os.symlink(str(target), str(link))
        self.assertIs(hd.do_clean(), True)
        self.assertTrue(os.path.islink(str(link)),
                        'clean が保護対象名の symlink を消した')

    def test_root_etc_symlink_refuses_clean(self):
        shutil.rmtree(str(self.hostdrv / 'etc'))
        other = self.root / 'conf'
        other.mkdir()
        os.symlink(str(other), str(self.hostdrv / 'etc'))
        (self.hostdrv / 'keep.bin').write_bytes(b'x')
        self.assertIs(hd.do_clean(), False, 'etc が symlink でも clean が通った')
        self.assertTrue((self.hostdrv / 'keep.bin').exists(),
                        '拒否したのに消していた')

    def test_protected_directory_contents_are_kept(self):
        stale = self.hostdrv / 'etc' / 'settings.db'
        stale.mkdir()
        (stale / 'inner').write_bytes(b'inner')
        (self.hostdrv / 'top.bin').write_bytes(b'drop')
        self.assertIs(hd.do_clean(), True)
        self.assertTrue((stale / 'inner').exists(),
                        'bottom-up が保護ディレクトリの中身を先に消した')
        self.assertFalse((self.hostdrv / 'top.bin').exists())

    def test_unrelated_symlink_is_removed(self):
        (self.hostdrv / 'bin').mkdir()
        os.symlink(str(self.root / 'nowhere'),
                   str(self.hostdrv / 'bin' / 'link'))
        self.assertIs(hd.do_clean(), True)
        self.assertFalse((self.hostdrv / 'bin').exists())


class ReviewB4EntryCheck(Base):
    """B4: 全体拒否と ENOENT 限定の徹底。"""

    def test_name_rule_path_still_checks_root_etc(self):
        alt = self.root / 'mnt3'
        (alt / 'conf').mkdir(parents=True)
        os.symlink(str(alt / 'conf'), str(alt / 'etc'))
        with self.assertRaises(protect.ProtectError):
            # 名前規則だけで True にできる問い合わせでも前提検査は通る
            protect.is_protected(str(alt), str(alt / 'etc' / 'settings.db'))

    def test_rm_refuses_when_root_etc_is_symlink(self):
        alt = self.root / 'mnt4'
        (alt / 'conf').mkdir(parents=True)
        os.symlink(str(alt / 'conf'), str(alt / 'etc'))
        self._patch(nd, 'MOUNT_POINT', str(alt))
        self.assertIs(nd.do_rm('/etc/settings.db'), False)

    def test_etc_as_regular_file_fails(self):
        alt = self.root / 'mnt5'
        alt.mkdir()
        (alt / 'etc').write_bytes(b'not a dir')
        with self.assertRaises(protect.ProtectError):
            protect.check_root_etc(str(alt))
        with self.assertRaises(protect.ProtectError):
            protect.is_protected(str(alt), str(alt / 'sh.bin'))

    def test_unreadable_etc_fails(self):
        """/etc を列挙できない = 実体規則を当てられない → 配備を失敗させる。"""
        if os.geteuid() == 0:
            self.skipTest('root では EACCES を作れない')
        existing = self.mount / 'bin' / 'old.bin'
        existing.write_bytes(b'x')          # 宛先が既存 = 実体規則まで進む
        os.chmod(str(self.mount / 'etc'), 0o000)
        try:
            with self.assertRaises(protect.ProtectError):
                protect.is_protected(str(self.mount), str(existing))
        finally:
            os.chmod(str(self.mount / 'etc'), 0o755)


class ReviewB6FailurePropagation(Base):
    """B6: mkdir / walk / rmdir の失敗を成功にしない。"""

    def test_mkdir_failure_fails_sync(self):
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)
        self.manifest([], directories=['/opt/new'])
        self.pairs({})
        self.fake.fail_mkdir = True
        self.assertIs(nd.do_sync(), False)

    def test_mkdir_failure_fails_copy(self):
        self.fake.fail_mkdir = True
        self.assertIs(nd.do_copy([str(self.src_bin)], dest_dir='/newdir'),
                      False)

    def test_walk_error_fails_sync_from_hostdrv(self):
        (self.hostdrv / 'bin').mkdir()
        (self.hostdrv / 'bin' / 'sh.bin').write_bytes(b'NEWBIN')
        real_walk = os.walk

        def broken(top, *a, **kw):
            onerror = kw.get('onerror')
            for item in real_walk(top, *a, **kw):
                yield item
            if onerror:
                onerror(OSError(errno.EACCES, 'denied', str(top)))
        self._patch(nd.os, 'walk', broken)
        self.assertIs(nd.do_sync_from_hostdrv(), False)

    def test_rmdir_failure_fails_clean(self):
        (self.hostdrv / 'bin').mkdir()
        (self.hostdrv / 'bin' / 'sh.bin').write_bytes(b'drop')
        real_rmdir = os.rmdir

        def boom(path):
            if str(path).endswith('bin'):
                raise OSError(errno.EACCES, 'denied')
            return real_rmdir(path)
        self._patch(hd.os, 'rmdir', boom)
        self.assertIs(hd.do_clean(), False)


class ReviewB8PullStamp(Base):
    """B8: 失敗した pull の全経路で来歴を残さない。"""

    def setUp(self):
        super(ReviewB8PullStamp, self).setUp()
        self.fake.mounted = False
        self.local = pathlib.Path(nd.NHD_LOCAL)
        self.remote = pathlib.Path(nd.NHD_REMOTE)
        self.remote.parent.mkdir(parents=True, exist_ok=True)
        self.remote.write_bytes(b'REMOTE-IMAGE' * 16)
        self._patch(nd, 'do_mount', lambda: True)

    def test_missing_remote_removes_stale_stamp(self):
        self.assertIs(nd.do_pull(), True)
        self.remote.unlink()
        self.assertIs(nd.do_pull(), False)
        self.assertFalse(os.path.isfile(nd.stamp_path()))

    def test_mounted_refusal_removes_stale_stamp(self):
        self.assertIs(nd.do_pull(), True)
        self.fake.mounted = True
        self.assertIs(nd.do_pull(), False)
        self.assertFalse(os.path.isfile(nd.stamp_path()))

    def test_mount_failure_after_copy_leaves_no_stamp(self):
        self._patch(nd, 'do_mount', lambda: False)
        self.assertIs(nd.do_pull(), False)
        self.assertFalse(os.path.isfile(nd.stamp_path()),
                         'do_mount 失敗なのに来歴が残った')
        self.assertIs(nd.do_deploy(), False)

    def test_ensure_local_missing_remote_removes_stale_stamp(self):
        self.assertIs(nd.do_pull(), True)
        self.local.unlink()
        self.remote.unlink()
        self.assertIs(nd.ensure_local_nhd(), False)
        self.assertFalse(os.path.isfile(nd.stamp_path()))


class ReviewB9RootNormalization(Base):
    """B9: root へ戻り切る正規化結果は正当。"""

    def test_normalize_to_root(self):
        self.assertEqual(protect.normalize_guest_path('/bin/..'), '/')
        self.assertEqual(protect.normalize_guest_path('/'), '/')
        self.assertEqual(protect.normalize_guest_path('/a/b/../..'), '/')

    def test_escape_above_root_still_refused(self):
        with self.assertRaises(protect.ProtectError):
            protect.normalize_guest_path('/..')
        with self.assertRaises(protect.ProtectError):
            protect.normalize_guest_path('/a/../..')

    def test_copy_dest_dotdot_to_root(self):
        ok = nd.do_copy([str(self.src_bin)], dest_dir='/bin/..')
        self.assertIs(ok, True)
        self.assertTrue((self.mount / 'sh.bin').exists())

    def test_manifest_guest_with_dotdot_in_parent(self):
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)
        self.manifest([{'host': 'build/sh.bin', 'guest': '/bin/../motd',
                        'tags': ['core']}])
        self.pairs({'build/sh.bin': [(str(self.src_bin), '/bin/../motd')]})
        self.assertIs(nd.do_sync(), True)
        self.assertEqual((self.mount / 'motd').read_bytes(),
                         self.src_bin.read_bytes())


class ManifestEntryPoints(Base):
    """resolver を差し替えずに manifest の入口 (file / glob / tag) を通す。"""

    def setUp(self):
        super(ManifestEntryPoints, self).setUp()
        self._patch(nd, 'PROJ_DIR', str(self.root))
        self._patch(hd, 'PROJ_DIR', str(self.root))
        self._patch(nd, 'do_write_boot', lambda p: True)
        self._patch(nd, 'ensure_local_nhd', lambda: True)

    def test_glob_entry_into_etc_directory(self):
        digest = sha256(self.db)
        self.manifest([{'host': 'build/*.db', 'guest': '/etc/',
                        'type': 'glob', 'tags': ['core']},
                       {'host': 'build/defaults.tsv',
                        'guest': '/etc/settings.tsv', 'tags': ['core']}])
        self.assertIs(nd.do_sync(), True)
        self.assertDbIntact(digest)
        self.assertEqual((self.mount / 'etc' / 'settings.tsv').read_bytes(),
                         self.src_tsv.read_bytes())
        self.assertNoProtectedWrites()

    def test_tag_filter_still_protects(self):
        digest = sha256(self.db)
        self.manifest([{'host': 'build/settings.db', 'guest': '/etc/',
                        'tags': ['core']},
                       {'host': 'build/sh.bin', 'guest': '/bin/',
                        'tags': ['userland']}])
        self.assertIs(nd.do_sync(tag_filter='core'), True)
        self.assertDbIntact(digest)
        self.assertFalse((self.mount / 'bin' / 'sh.bin').exists())
        self.assertNoProtectedWrites()

    def test_hostdrv_glob_entry(self):
        stale = self.hostdrv / 'etc' / 'settings.db'
        stale.write_bytes(b'OLD')
        digest = sha256(stale)
        self.manifest([{'host': 'build/*.db', 'guest': '/etc/',
                        'type': 'glob', 'tags': ['core']}])
        self.assertIs(hd.do_sync(), True)
        self.assertEqual(sha256(stale), digest)


# ======================================================================
#  (4) main の終了コード
# ======================================================================
class MainExit(Base):

    def _main(self, argv):
        saved = sys.argv
        sys.argv = ['nhd_deploy.py'] + argv
        try:
            return nd.main()
        finally:
            sys.argv = saved

    def test_failing_sync_returns_false(self):
        self._patch(nd, 'do_sync', lambda tag_filter=None: False)
        self.assertIs(self._main(['sync']), False)

    def test_failing_copy_returns_false(self):
        self._patch(nd, 'do_copy',
                    lambda *a, **k: False)
        self.assertIs(self._main(['copy', str(self.src_bin)]), False)

    def test_failing_rm_returns_false(self):
        self._patch(nd, 'do_rm', lambda f: False)
        self.assertIs(self._main(['rm', '/bin/old.bin']), False)

    def test_failing_umount_returns_false(self):
        self._patch(nd, 'do_umount', lambda: False)
        self.assertIs(self._main(['umount']), False)

    def test_unknown_command_returns_false(self):
        self.assertIs(self._main(['nonsense']), False)

    def test_successful_sync_returns_true(self):
        self._patch(nd, 'do_sync', lambda tag_filter=None: True)
        self.assertIs(self._main(['sync']), True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
