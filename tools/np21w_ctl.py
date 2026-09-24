#!/usr/bin/env python3
"""
np21w_ctl.py — NP21/W の停止・起動・起動完了待ち・状態表示

NP21/W を落とした直後に起動すると、前のプロセス (または Windows の後始末) が
os32.nhd / ISO / FD イメージを握ったままで、新しいプロセスがそれを開けずに
起動が途中で止まることが繰り返し起きた (2026-09-24〜25)。この道具は起動の前に

  1. np21 のプロセスが 0 であることを確かめ (残っていれば待ち、時間切れで失敗)
  2. 使う媒体が Windows 側から排他で**続けて 5 秒** (--stable、1 秒おき) 開けるまで待ち
     (`[IO.File]::Open(path,'Open','ReadWrite','None')`。途中で 1 回でもロックされたら
     数え直し。時間切れでロック中のファイルを名指しして失敗)
  3. それから Start-Process で起動し
  4. プロセスが 10 秒 (--alive) 生きていて /api/status が応答することを確かめる
     (途中で消えたら「起動直後に終了した」で失敗)

という順を必ず踏む。ini は**読むだけ**で書き換えない ([D2])。

使い方:
  python3 tools/np21w_ctl.py stop [--timeout 60]
  python3 tools/np21w_ctl.py start --ini np21x64w.ini [--fd os32_boot.d88]
                                   [--exe np21x64w.exe] [--timeout 60] [--wait-ready]
                                   [--stable 5] [--alive 10] [--api-timeout 60]
  python3 tools/np21w_ctl.py wait-ready [--timeout 180]
  python3 tools/np21w_ctl.py status [--ini np21x64w.ini]

  --ini / --fd / --exe は NP21W_DIR 直下の**名前**で渡す (パスは受け付けない)。

ロック待ちの対象:
  ini の HDD1FILE〜HDD4FILE、CD1_FILE〜CD4_FILE、FDD1FILE〜FDD4FILE の空でない値と、
  --fd の媒体。ini が読めなければ既定の os32.nhd / os32_install.iso / os32_boot.d88。
  ini に書かれているが存在しない媒体は警告して待たない (--fd の媒体が無ければ失敗)。

NP21W_DIR: 環境変数 → .env → .env.sample の順 (nhd_deploy.py / np21w_restart.py と同じ)。
Windows 表記は WIN_NP21W_DIR で上書きできる。.env の中身は出力しない ([D3])。

終了コード: 0 = 成功、1 = 失敗 (プロセスが残る・ロックが解けない・起動直後に終了した・
            aidebug が応答しない・起動完了しない)、
            2 = 引数や設定の誤り。

試験: tools/tests/test_np21w_ctl.py (Windows 側の操作は WinOps を偽物に差し替える)。
"""

import argparse
import base64
import csv
import io
import os
import re
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(TOOLS_DIR)

DEFAULT_EXE = 'np21x64w.exe'
DEFAULT_MEDIA = ('os32.nhd', 'os32_install.iso', 'os32_boot.d88')
# ini の媒体キー。NP21/W (ai-debug fork) の np21x64w.ini に実在する綴り。
MEDIA_KEYS = tuple(['HDD%dFILE' % i for i in range(1, 5)] +
                   ['CD%d_FILE' % i for i in range(1, 5)] +
                   ['FDD%dFILE' % i for i in range(1, 5)])
READY_TEXT = 'Waiting for commands'   # userland/shell/rshell.c の起動完了行
NAME_RE = re.compile(r'[A-Za-z0-9_.-]+')

WIN_SYS = '/mnt/c/Windows/System32'
TASKLIST = WIN_SYS + '/tasklist.exe'
TASKKILL = WIN_SYS + '/taskkill.exe'
POWERSHELL = WIN_SYS + '/WindowsPowerShell/v1.0/powershell.exe'


class CtlError(Exception):
    """利用者に見せる誤り。code は終了コード。"""
    def __init__(self, msg, code=1):
        Exception.__init__(self, msg)
        self.code = code


# ---------------------------------------------------------------------------
# パス
# ---------------------------------------------------------------------------
def resolve_np21w_dir(environ=None, proj_dir=PROJ_DIR):
    """NP21W_DIR (WSL パス) を 環境変数 → .env → .env.sample の順で決める。"""
    environ = os.environ if environ is None else environ
    if environ.get('NP21W_DIR'):
        return environ['NP21W_DIR']
    for env_file in ('.env', '.env.sample'):
        env_path = os.path.join(proj_dir, env_file)
        if os.path.isfile(env_path):
            with open(env_path, 'r', errors='replace') as f:
                for line in f:
                    line = line.strip()
                    if line.startswith('NP21W_DIR='):
                        return line.split('=', 1)[1].strip()
    return '/tmp/np21w'


def to_win_path(wsl_path):
    """/mnt/c/x/y → C:\\x\\y。それ以外はそのまま返す。"""
    if wsl_path.startswith('/mnt/') and len(wsl_path) >= 6:
        drive = wsl_path[5].upper()
        rest = wsl_path[6:].replace('/', '\\')
        return '%s:%s' % (drive, rest or '\\')
    return wsl_path


def check_name(name, what):
    if (not name or not NAME_RE.fullmatch(name) or name.startswith('.')
            or '..' in name):
        raise CtlError('%s は NP21W_DIR 直下の名前で渡す (パス不可): %r'
                       % (what, name), 2)
    return name


class Paths(object):
    def __init__(self, wsl_dir, win_dir=None):
        self.wsl = wsl_dir.rstrip('/')
        self.win = (win_dir or to_win_path(self.wsl)).rstrip('\\')

    def wsl_of(self, name):
        return self.wsl + '/' + name

    def win_of(self, name):
        return self.win + '\\' + name


def paths_from_env(environ=None):
    environ = os.environ if environ is None else environ
    return Paths(resolve_np21w_dir(environ), environ.get('WIN_NP21W_DIR'))


# ---------------------------------------------------------------------------
# ini (読むだけ)
# ---------------------------------------------------------------------------
def ini_media(ini_path):
    """ini の媒体キーの空でない値 (Windows パス) を並べて返す。

    読めなければ None。ini は CP932 なので bytes で読んで cp932 で解く。
    同じ値は 1 回だけ。
    """
    try:
        with open(ini_path, 'rb') as f:
            raw = f.read()
    except OSError:
        return None
    text = raw.decode('cp932', errors='replace')
    found = []
    for line in text.splitlines():
        if '=' not in line:
            continue
        key, value = line.split('=', 1)
        if key.strip().upper() in MEDIA_KEYS:
            value = value.strip()
            if value and value not in found:
                found.append(value)
    return found


def media_targets(paths, ini_name, fd_name, warn):
    """待つ対象 [(Windows パス, 必須か)]。"""
    targets = []
    listed = ini_media(paths.wsl_of(ini_name)) if ini_name else None
    if listed is None:
        if ini_name:
            warn('ini %s が読めない — 既定の %s を待つ'
                 % (ini_name, ' / '.join(DEFAULT_MEDIA)))
        listed = [paths.win_of(n) for n in DEFAULT_MEDIA]
    for p in listed:
        targets.append((p, False))
    if fd_name:
        fd = paths.win_of(fd_name)
        targets = [t for t in targets if t[0].lower() != fd.lower()]
        targets.append((fd, True))
    return targets


# ---------------------------------------------------------------------------
# Windows 側の操作 (試験では偽物に差し替える)
# ---------------------------------------------------------------------------
def _ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


LOCK_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$paths = @(%s)
for ($i = 0; $i -lt $paths.Count; $i++) {
  $p = $paths[$i]
  if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { "missing`t$i"; continue }
  try { $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close(); "free`t$i" }
  catch { "locked`t$i" }
}
"""

START_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$p = Start-Process -FilePath %s -ArgumentList @(%s) -WorkingDirectory %s -PassThru
"pid`t$($p.Id)"
"""


class WinOps(object):
    """実際の Windows 側コマンド。WSL の /mnt/c から呼ぶ。"""

    def _run(self, args, timeout=60):
        # UNC なカレント (\\wsl$\...) から Windows の exe を呼ぶと警告や
        # 引数崩れが起きるので、カレントを /mnt/c に置く。
        try:
            p = subprocess.run(args, capture_output=True, timeout=timeout,
                               cwd='/mnt/c' if os.path.isdir('/mnt/c') else None)
        except FileNotFoundError:
            raise CtlError('%s が見つからない (WSL から Windows を呼べない環境)'
                           % args[0], 2)
        except subprocess.TimeoutExpired:
            raise CtlError('%s が %d 秒で返らない' % (os.path.basename(args[0]),
                                                  timeout))
        return p.returncode, p.stdout.decode('utf-8', 'replace').replace('\r', '')

    def _ps(self, script, timeout=60):
        enc = base64.b64encode(script.encode('utf-16-le')).decode('ascii')
        return self._run([POWERSHELL, '-NoProfile', '-NonInteractive',
                          '-EncodedCommand', enc], timeout)

    def list_processes(self):
        """[(image, pid)] — 名前に np21 を含むもの。"""
        rc, out = self._run([TASKLIST, '/FO', 'CSV', '/NH'])
        if rc != 0:
            raise CtlError('tasklist が失敗した (rc=%d)' % rc)
        rows = []
        for row in csv.reader(io.StringIO(out)):
            if len(row) >= 2 and 'np21' in row[0].lower():
                try:
                    rows.append((row[0], int(row[1])))
                except ValueError:
                    pass
        return rows

    def kill(self, pid):
        self._run([TASKKILL, '/F', '/PID', str(pid)])

    def probe(self, win_paths):
        """{Windows パス: 'free' | 'locked' | 'missing'}"""
        script = LOCK_SCRIPT % ', '.join(_ps_quote(p) for p in win_paths)
        rc, out = self._ps(script)
        # 結果はパスではなく添字で返させる。PowerShell の標準出力は CP932 で、
        # 日本語のパスを文字列で往復させると化ける (実測 2026-09-25)。
        result = {}
        for line in out.splitlines():
            parts = line.split('\t')
            if (len(parts) == 2 and parts[0] in ('free', 'locked', 'missing')
                    and parts[1].isdigit() and int(parts[1]) < len(win_paths)):
                result[win_paths[int(parts[1])]] = parts[0]
        for p in win_paths:
            # 何も返らなかったものは「開けたと確認できなかった」= locked 扱い
            result.setdefault(p, 'locked')
        return result

    def start(self, exe, args, cwd):
        script = START_SCRIPT % (_ps_quote(exe),
                                 ', '.join(_ps_quote('"%s"' % a) for a in args),
                                 _ps_quote(cwd))
        rc, out = self._ps(script)
        m = re.search(r'pid\t(\d+)', out)
        if rc != 0 or not m:
            raise CtlError('Start-Process が失敗した (rc=%d): %s'
                           % (rc, out.strip()[:200]))
        return int(m.group(1))


def http_tvram(timeout=15):
    """ゲスト画面のテキスト。届かなければ None。"""
    sys.path.insert(0, os.path.join(TOOLS_DIR, 'np21w_mcp'))
    import np21w_client as emu
    try:
        return emu.get('/api/tvram', timeout=timeout).decode('utf-8', 'replace')
    except Exception:
        emu._direct = None   # 起動直後は届かない。次回は経路を測り直す
        return None


def http_status(timeout=15):
    sys.path.insert(0, os.path.join(TOOLS_DIR, 'np21w_mcp'))
    import np21w_client as emu
    try:
        return emu.get('/api/status', timeout=timeout).decode('utf-8', 'replace')
    except Exception:
        emu._direct = None
        return None


# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------
class Ctl(object):
    def __init__(self, paths, ops=None, tvram=http_tvram, status_fn=http_status,
                 sleep=time.sleep, clock=time.monotonic, out=None, err=None,
                 poll=1.0, settle=2.0):
        self.paths = paths
        self.ops = ops or WinOps()
        self.tvram = tvram
        self.status_fn = status_fn
        self.sleep = sleep
        self.clock = clock
        self.out = out or sys.stdout
        self.err = err or sys.stderr
        self.poll = poll
        self.settle = settle

    def say(self, msg):
        self.out.write(msg + '\n')
        self.out.flush()

    def warn(self, msg):
        self.err.write('np21w_ctl: ' + msg + '\n')
        self.err.flush()

    def _until(self, timeout, done):
        """done() が真になるまで poll 間隔で呼ぶ。最後の done() の値を返す。"""
        end = self.clock() + timeout
        while True:
            value = done()
            if value or self.clock() >= end:
                return value
            self.sleep(self.poll)

    # -- プロセス ----------------------------------------------------------
    def wait_no_process(self, timeout):
        if self._until(timeout, lambda: not self.ops.list_processes()):
            return
        left = self.ops.list_processes()
        raise CtlError('NP21/W のプロセスが %d 秒たっても残っている: %s'
                       % (timeout, ', '.join('%s pid=%d' % r for r in left)))

    def stop(self, timeout=60):
        procs = self.ops.list_processes()
        if not procs:
            self.say('stopped (動いていなかった)')
            return 0
        for image, pid in procs:
            self.say('taskkill /F %s pid=%d' % (image, pid))
            self.ops.kill(pid)
        self.wait_no_process(timeout)
        self.say('stopped')
        return 0

    # -- 媒体のロック -------------------------------------------------------
    def wait_media(self, targets, timeout, stable=5.0):
        """各媒体が**続けて stable 秒**開けることを確かめる。

        1 回開けただけでは通さない。nhd-pull でコピーした直後に Windows 側
        (Defender の走査など) が一時的に掴み直し、1 回だけのプローブがその
        隙間を「開ける」と判断して、起動した NP21/W がすぐ終了した (2026-09-25)。
        途中で 1 回でもロックされたら、その媒体は数え直す。
        """
        required = dict(targets)
        state = {}
        free_since = {}
        last_free = {}
        missing = set()

        def settled(p):
            # 「開けた」と実際に見た最初と最後の間が stable 秒以上。
            # 見ていない時間 (最後のプローブから今まで) は数えない
            return p in missing or (
                p in free_since and last_free[p] - free_since[p] >= stable)

        def check():
            pending = [p for p in required if not settled(p)]
            now = self.clock()
            for p, st in self.ops.probe(pending).items():
                state[p] = st
                if st == 'missing':
                    missing.add(p)
                elif st == 'free':
                    free_since.setdefault(p, now)
                    last_free[p] = now
                else:
                    if p in free_since:
                        self.say('locked again (数え直し): %s' % p)
                    free_since.pop(p, None)
            return all(settled(p) for p in required)

        ok = self._until(timeout, check)
        for p in required:
            if p in missing:
                if required[p]:
                    raise CtlError('媒体が無い: %s' % p, 2)
                self.warn('ini にある媒体が無い (待たない): %s' % p)
        if not ok:
            locked = [p for p in required if state.get(p) == 'locked']
            unsteady = [p for p in required
                        if p not in missing and p not in locked and not settled(p)]
            msg = '媒体が %d 秒たっても %g 秒続けて開けない:' % (timeout, stable)
            if locked:
                msg += '\n  locked: ' + '\n  locked: '.join(locked)
            if unsteady:
                msg += '\n  不安定 (開けたり掴まれたり): ' + \
                       '\n  不安定 (開けたり掴まれたり): '.join(unsteady)
            raise CtlError(msg)
        for p in required:
            if p not in missing:
                self.say('free %gs: %s' % (stable, p))

    # -- 起動 ----------------------------------------------------------------
    def start(self, ini, fd=None, exe=DEFAULT_EXE, timeout=60,
              wait_ready=False, ready_timeout=180, stable=5.0,
              alive=10.0, api_timeout=60.0):
        check_name(ini, '--ini')
        check_name(exe, '--exe')
        if fd:
            check_name(fd, '--fd')
        if not os.path.isfile(self.paths.wsl_of(exe)):
            raise CtlError('exe が無い: %s' % self.paths.wsl_of(exe), 2)
        if not os.path.isfile(self.paths.wsl_of(ini)):
            raise CtlError('ini が無い: %s' % self.paths.wsl_of(ini), 2)

        self.wait_no_process(timeout)
        targets = media_targets(self.paths, ini, fd, self.warn)
        self.wait_media(targets, timeout, stable)
        # 開けた直後に閉じた自分のハンドルの後始末を待つ (仮スクリプトと同じ 2 秒)
        if self.settle:
            self.sleep(self.settle)
        args = ['/i' + self.paths.win_of(ini)]
        if fd:
            args.append(self.paths.win_of(fd))
        pid = self.ops.start(self.paths.win_of(exe), args, self.paths.win)
        self.say('started %s pid=%d (%s)' % (exe, pid, ' '.join(args)))
        self.verify_alive(pid, alive, api_timeout)
        if wait_ready:
            return self.wait_ready(ready_timeout)
        return 0

    def verify_alive(self, pid, alive=10.0, api_timeout=60.0):
        """起動したプロセスが alive 秒生きていて、/api/status が応答するまで待つ。

        途中でプロセスが消えたら「起動直後に終了した」で失敗する。
        """
        if alive <= 0 and api_timeout <= 0:
            return
        begin = self.clock()
        end = begin + max(alive, api_timeout)
        api = None
        while True:
            pids = [q for _, q in self.ops.list_processes()]
            if pid not in pids:
                raise CtlError('起動直後に終了した (pid=%d、起動から %.0f 秒)。'
                               '媒体が開けなかった可能性 — status で媒体を確かめる'
                               % (pid, self.clock() - begin))
            if api is None:
                api = self.status_fn()
            lived = self.clock() - begin >= alive
            if lived and api:
                self.say('alive %gs pid=%d, aidebug up' % (alive, pid))
                return
            if self.clock() >= end:
                raise CtlError('pid=%d は生きているが aidebug (/api/status) が '
                               '%g 秒応答しない (ini の aidebug=true / aidbport)'
                               % (pid, api_timeout))
            self.sleep(self.poll)

    # -- 起動完了 ------------------------------------------------------------
    def wait_ready(self, timeout=180):
        seen = {'text': None}

        def ready():
            text = self.tvram()
            if text is not None:
                seen['text'] = text
            return text is not None and READY_TEXT in text

        if self._until(timeout, ready):
            self.say('ready (%s)' % READY_TEXT)
            return 0
        if seen['text'] is None:
            raise CtlError('%d 秒たっても aidebug API に届かない '
                           '(NP21/W が動いているか、ini の aidebug=true / aidbport)'
                           % timeout)
        tail = [l for l in seen['text'].splitlines() if l.strip()][-5:]
        raise CtlError('%d 秒たっても "%s" が出ない。画面の最後:\n  %s'
                       % (timeout, READY_TEXT, '\n  '.join(tail)))

    # -- 状態 ----------------------------------------------------------------
    def status(self, ini=None):
        procs = self.ops.list_processes()
        if procs:
            for image, pid in procs:
                self.say('process: %s pid=%d' % (image, pid))
        else:
            self.say('process: none')
        st = self.status_fn()
        self.say('aidebug: %s' % ('up ' + st.strip()[:200] if st else 'down'))
        if ini:
            check_name(ini, '--ini')
        targets = media_targets(self.paths, ini, None, self.warn)
        state = self.ops.probe([p for p, _ in targets])
        for p, _ in targets:
            self.say('%-7s %s' % (state.get(p, '?') + ':', p))
        return 0


def build_parser():
    ap = argparse.ArgumentParser(
        prog='np21w_ctl.py',
        description='NP21/W の停止・起動 (媒体のロック解除を待つ)・起動完了待ち・状態')
    sub = ap.add_subparsers(dest='cmd')
    sub.required = True
    p = sub.add_parser('stop', help='np21 のプロセスを taskkill /F で止め、消えるまで待つ')
    p.add_argument('--timeout', type=float, default=60)
    p = sub.add_parser('start', help='プロセス 0・媒体の解放を確かめてから起動する')
    p.add_argument('--ini', required=True, help='NP21W_DIR 直下の ini の名前')
    p.add_argument('--fd', help='FD 引数で渡すイメージの名前')
    p.add_argument('--exe', default=DEFAULT_EXE)
    p.add_argument('--timeout', type=float, default=60,
                   help='プロセス消滅・ロック解除のそれぞれの上限秒 (既定 60)')
    p.add_argument('--wait-ready', action='store_true',
                   help='起動後に wait-ready まで続ける')
    p.add_argument('--ready-timeout', type=float, default=180)
    p.add_argument('--stable', type=float, default=5,
                   help='各媒体が続けて開けることを確かめる秒数 (既定 5、1 秒おき)')
    p.add_argument('--alive', type=float, default=10,
                   help='起動後にプロセスが生きていることを見る秒数 (既定 10)')
    p.add_argument('--api-timeout', type=float, default=60,
                   help='起動後に /api/status の応答を待つ上限秒 (既定 60)')
    p = sub.add_parser('wait-ready', help='/api/tvram に "%s" が出るまで待つ' % READY_TEXT)
    p.add_argument('--timeout', type=float, default=180)
    p = sub.add_parser('status', help='プロセス・aidebug・媒体のロック状態')
    p.add_argument('--ini', default=None, help='媒体を拾う ini (既定: 既定の 3 つ)')
    return ap


def main(argv=None, ctl_factory=None):
    args = build_parser().parse_args(argv)
    ctl = (ctl_factory or (lambda: Ctl(paths_from_env())))()
    try:
        if args.cmd == 'stop':
            return ctl.stop(args.timeout)
        if args.cmd == 'start':
            return ctl.start(args.ini, args.fd, args.exe, args.timeout,
                             args.wait_ready, args.ready_timeout, args.stable,
                             args.alive, args.api_timeout)
        if args.cmd == 'wait-ready':
            return ctl.wait_ready(args.timeout)
        return ctl.status(args.ini)
    except CtlError as exc:
        ctl.warn(str(exc))
        return exc.code


if __name__ == '__main__':
    sys.exit(main())
