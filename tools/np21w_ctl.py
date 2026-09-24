#!/usr/bin/env python3
"""
np21w_ctl.py — NP21/W の停止・起動・起動完了待ち・FD の出し入れ・状態表示

停止は NP21/W 自身に頼む (ai-debug フォークの `/api/instance` → `/api/quit`)。
API が応答しないときだけ強制終了に落とし、その対象は **NP21W_DIR の exe と
ExecutablePath が一致するプロセス**に限る。名前に np21 を含むだけのものや、別の
場所に入っている NP21/W には触らない。

起動は、媒体のロックで起動が途中で止まる事故 (docs/POLICY_DEBUG.md §4-60) を
避けるため、次の順を必ず踏む。ini は**読むだけ**で書き換えない ([D2])。

  1. この exe のプロセスが無いことを確かめる (残っていれば待ち、時間切れで失敗)
  2. 使う媒体が Windows 側から排他で**続けて --stable 秒** (既定 5、1 秒おき) 開ける
     まで待つ。安定した媒体も起動まで毎回プローブし続け、1 回でもロックされたら
     その媒体は数え直す。全部安定したら 2 秒置き、**起動の直前に全部をもう一度**
     プローブする。全体の期限 (--timeout) を過ぎた結果は成功に数えない
  3. Start-Process で起動する
  4. プロセスが --alive 秒生きていて、`/api/instance` の pid と exe が Start-Process の
     pid と NP21W_DIR の exe に一致することを確かめる。API が応答しない・進まない
     ときは `/api/dialog` を見て、モーダルのダイアログが出ていればその本文を出して
     失敗する。成功を返す直前にもう一度 pid の生存を確かめる

使い方:
  python3 tools/np21w_ctl.py stop [--exe np21x64w.exe] [--timeout 60]
  python3 tools/np21w_ctl.py start --ini np21x64w.ini [--fd os32_boot.d88]
                                   [--exe np21x64w.exe] [--timeout 60] [--wait-ready]
                                   [--stable 5] [--alive 10] [--api-timeout 60]
  python3 tools/np21w_ctl.py wait-ready [--timeout 180]
  python3 tools/np21w_ctl.py fdd --drive 1 (--insert <name> [--readonly] | --eject)
  python3 tools/np21w_ctl.py status [--ini np21x64w.ini] [--exe np21x64w.exe]

  --ini / --fd / --exe / --insert は NP21W_DIR 直下の**名前**で渡す (パスは不可)。
  --api-timeout 0 は「API を待たない」(プロセスの生存だけを --alive 秒見る)。
  --timeout は --stable 以上であること (満たさなければ引数の誤り)。

ロック待ちの対象:
  ini の [NekoProject21] 節の HDD1FILE〜HDD4FILE、CD1_FILE〜CD4_FILE、
  FDD1FILE〜FDD4FILE、SCSIHDD0〜SCSIHDD3 の空でない値と --fd の媒体。値は
  NP21/W (common/profile.c) と同じ規則で読む: 前後の空白を取り、両端が `"` なら
  1 組だけ外してもう一度空白を取る。節の中で同じキーが複数あれば**最初**が効く。
  ini が読めなければ既定の os32.nhd / os32_install.iso / os32_boot.d88。
  ini にあるが存在しない媒体は警告して待たない (--fd の媒体が無ければ失敗)。

NP21W_DIR: 環境変数 → .env → .env.sample の順 (nhd_deploy.py と同じ)。
Windows 表記は WIN_NP21W_DIR で上書きできる。.env の中身は出力しない ([D3])。
aidebug の URL は NP21W_AIDEBUG_URL (既定 http://127.0.0.1:8025)。

終了コード: 0 = 成功、1 = 失敗 (プロセスが残る・ロックが解けない・プローブが壊れた・
            起動直後に終了した・ダイアログが出ている・API が応答しない・起動完了しない)、
            2 = 引数や設定の誤り。

試験: tools/tests/test_np21w_ctl.py (Windows 側の操作は WinOps、HTTP は Http を偽物に
差し替える)。
"""

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(TOOLS_DIR)

DEFAULT_EXE = 'np21x64w.exe'
DEFAULT_MEDIA = ('os32.nhd', 'os32_install.iso', 'os32_boot.d88')
INI_SECTION = 'NekoProject21'     # win9x/ini.cpp s_szIniTitle (SUPPORT_PC9821)
# ini の媒体キー (win9x/ini.cpp の s_IniItems と同じ綴り)
MEDIA_KEYS = tuple(['HDD%dFILE' % i for i in range(1, 5)] +
                   ['CD%d_FILE' % i for i in range(1, 5)] +
                   ['FDD%dFILE' % i for i in range(1, 5)] +
                   ['SCSIHDD%d' % i for i in range(0, 4)])
READY_TEXT = 'Waiting for commands'   # userland/shell/rshell.c の起動完了行
NAME_RE = re.compile(r'[A-Za-z0-9_.-]+')
OLD_FORK = ('NP21/W のフォークが古い (/api/instance が無い)。'
            'np21w-src で make build → NP21/W を止めて make deploy が要る')

WIN_SYS = '/mnt/c/Windows/System32'
TASKKILL = WIN_SYS + '/taskkill.exe'
POWERSHELL = WIN_SYS + '/WindowsPowerShell/v1.0/powershell.exe'
CURL = WIN_SYS + '/curl.exe'
AIDEBUG_URL = os.environ.get('NP21W_AIDEBUG_URL', 'http://127.0.0.1:8025')


class CtlError(Exception):
    """利用者に見せる誤り。code は終了コード。"""
    def __init__(self, msg, code=1):
        Exception.__init__(self, msg)
        self.code = code


class ProbeError(CtlError):
    """ロックの検査そのものが壊れた (ロックされているのとは別)。"""


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


def win_norm(path):
    """Windows パスの比較用 (大文字小文字・区切り・末尾を揃える)。"""
    if not path:
        return ''
    return path.replace('/', '\\').rstrip('\\').lower()


def is_win_abs(path):
    return bool(re.match(r'^[A-Za-z]:\\', path)) or path.startswith('\\\\')


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
# ini (読むだけ。NP21/W の common/profile.c と同じ解釈)
# ---------------------------------------------------------------------------
def _ascii_upper(s):
    return ''.join(chr(ord(c) - 32) if 'a' <= c <= 'z' else c for c in s)


def decode_ini(raw):
    """textcnv と同じく BOM を見て、無ければ CP932。"""
    if raw.startswith(b'\xef\xbb\xbf'):
        return raw[3:].decode('utf-8', errors='replace')
    if raw.startswith(b'\xff\xfe'):
        return raw[2:].decode('utf-16-le', errors='replace')
    if raw.startswith(b'\xfe\xff'):
        return raw[2:].decode('utf-16-be', errors='replace')
    return raw.decode('cp932', errors='replace')


def _parse_line(line):
    """profile.c ParseLine: (名前, 値, 節か) か None。空白は ' ' だけを取る。"""
    s = line.strip(' ')
    if len(s) >= 2 and s[0] == '[' and s[-1] == ']':
        return s[1:-1].strip(' '), None, True
    if '=' not in s:
        return None
    key, value = s.split('=', 1)
    key = key.strip(' ')
    value = value.strip(' ')
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1].strip(' ')
    return key, value, False


def parse_ini(raw, keys=MEDIA_KEYS, section=INI_SECTION):
    """{キー(大文字): 値}。最初の該当節だけを読み、節内で最初に出た値を採る
    (profile.c SearchKey は該当節の次の節見出しで打ち切り、最初の一致で止まる)。"""
    text = decode_ini(raw)
    wanted = set(_ascii_upper(k) for k in keys)
    want_section = _ascii_upper(section)
    found = {}
    in_section = False
    for line in re.split(r'\r\n|\r|\n', text):
        parsed = _parse_line(line)
        if parsed is None:
            continue
        name, value, is_section = parsed
        if is_section:
            if in_section:
                break
            in_section = (_ascii_upper(name) == want_section)
            continue
        if in_section:
            k = _ascii_upper(name)
            if k in wanted and k not in found:
                found[k] = value
    return found


def ini_media(ini_path, np21w_win_dir=None):
    """ini の媒体キーの空でない値 (Windows パス) を ini の並びで。読めなければ None。

    相対パスは NP21W_DIR (起動時のカレント) からの相対として展開する。"""
    try:
        with open(ini_path, 'rb') as f:
            raw = f.read()
    except OSError:
        return None
    values = parse_ini(raw)
    out = []
    for key in MEDIA_KEYS:
        v = values.get(_ascii_upper(key), '')
        if not v:
            continue
        if not is_win_abs(v) and np21w_win_dir:
            v = np21w_win_dir.rstrip('\\') + '\\' + v
        if all(win_norm(v) != win_norm(o) for o in out):
            out.append(v)
    return out


def media_targets(paths, ini_name, fd_name, warn):
    """待つ対象 [(Windows パス, 必須か)] と、その出どころの説明。"""
    listed = ini_media(paths.wsl_of(ini_name), paths.win) if ini_name else None
    if listed is None:
        if ini_name:
            warn('ini %s が読めない — 既定の %s を待つ'
                 % (ini_name, ' / '.join(DEFAULT_MEDIA)))
        listed = [paths.win_of(n) for n in DEFAULT_MEDIA]
        source = 'default'
    else:
        source = 'ini ' + ini_name
    targets = [(p, False) for p in listed]
    if fd_name:
        fd = paths.win_of(fd_name)
        targets = [t for t in targets if win_norm(t[0]) != win_norm(fd)]
        targets.append((fd, True))
    return targets, source


# ---------------------------------------------------------------------------
# Windows 側の操作 (試験では偽物に差し替える)
# ---------------------------------------------------------------------------
def _ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def ps_encode(script):
    return base64.b64encode(script.encode('utf-16-le')).decode('ascii')


def ps_decode(encoded):
    return base64.b64decode(encoded).decode('utf-16-le')


# 結果は添字で返す (PowerShell の標準出力は CP932 で、日本語のパスは化ける)。
# ロック (IOException、共有違反を含む) と、検査の失敗 (それ以外の例外) を分ける。
LOCK_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$paths = @(%s)
for ($i = 0; $i -lt $paths.Count; $i++) {
  $p = $paths[$i]
  try {
    if (-not [IO.File]::Exists($p)) {
      if ([IO.Directory]::Exists($p)) { "error`t$i`tIsDirectory" } else { "missing`t$i" }
      continue
    }
    $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close(); "free`t$i"
  }
  catch [System.IO.IOException] { "locked`t$i" }
  catch { "error`t$i`t$($_.Exception.GetType().FullName)" }
}
'end'
"""

# 名前と ExecutablePath は UTF-8 の base64 で返す (日本語のパスでも化けない)。
PROC_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
function b64($s) { if ($s) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) } else { '-' } }
Get-CimInstance Win32_Process -Filter "Name LIKE '%np21%'" | ForEach-Object {
  "proc`t$($_.ProcessId)`t$(b64 $_.Name)`t$(b64 $_.ExecutablePath)"
}
'end'
"""

START_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$p = Start-Process -FilePath %s -ArgumentList @(%s) -WorkingDirectory %s -PassThru
"pid`t$($p.Id)"
"""


def lock_script(win_paths):
    return LOCK_SCRIPT % ', '.join(_ps_quote(p) for p in win_paths)


def start_script(exe, args, cwd):
    return START_SCRIPT % (_ps_quote(exe),
                           ', '.join(_ps_quote('"%s"' % a) for a in args),
                           _ps_quote(cwd))


class Proc(object):
    def __init__(self, pid, name, exe):
        self.pid = pid
        self.name = name
        self.exe = exe       # None = 読めなかった (他ユーザー・権限)

    def __repr__(self):
        return 'Proc(%d, %r, %r)' % (self.pid, self.name, self.exe)


def _b64s(s):
    if s == '-':
        return None
    try:
        return base64.b64decode(s).decode('utf-8', errors='replace')
    except (ValueError, TypeError):
        return None


class WinOps(object):
    """実際の Windows 側コマンド。WSL の /mnt/c から呼ぶ。"""

    def _run(self, args, timeout=60):
        """(rc, stdout, stderr)。UNC なカレントを避けて /mnt/c で動かす。"""
        try:
            p = subprocess.run(args, capture_output=True, timeout=timeout,
                               cwd='/mnt/c' if os.path.isdir('/mnt/c') else None)
        except FileNotFoundError:
            raise CtlError('%s が見つからない (WSL から Windows を呼べない環境)'
                           % args[0], 2)
        except subprocess.TimeoutExpired:
            raise CtlError('%s が %d 秒で返らない'
                           % (os.path.basename(args[0]), timeout))
        return (p.returncode,
                p.stdout.decode('utf-8', 'replace').replace('\r', ''),
                p.stderr.decode('utf-8', 'replace').replace('\r', ''))

    def _ps(self, script, timeout=60):
        return self._run([POWERSHELL, '-NoProfile', '-NonInteractive',
                          '-EncodedCommand', ps_encode(script)], timeout)

    def list_processes(self):
        """[Proc] — 名前に np21 を含むもの。exe は CIM の ExecutablePath。"""
        rc, out, err = self._ps(PROC_SCRIPT)
        lines = out.splitlines()
        if rc != 0 or 'end' not in lines:
            raise CtlError('プロセス一覧 (CIM) が失敗した (rc=%d): %s'
                           % (rc, err.strip()[:300]))
        procs = []
        for line in lines:
            parts = line.split('\t')
            if len(parts) == 4 and parts[0] == 'proc' and parts[1].isdigit():
                procs.append(Proc(int(parts[1]), _b64s(parts[2]) or '',
                                  _b64s(parts[3])))
        return procs

    def kill(self, pid):
        self._run([TASKKILL, '/F', '/PID', str(pid)])

    def probe(self, win_paths, timeout=60):
        """{Windows パス: 'free' | 'locked' | 'missing'}。検査が壊れたら ProbeError。"""
        if not win_paths:
            return {}
        rc, out, err = self._ps(lock_script(win_paths), timeout=max(1, timeout))
        return parse_probe(win_paths, rc, out, err)

    def start(self, exe, args, cwd):
        rc, out, err = self._ps(start_script(exe, args, cwd))
        m = re.search(r'pid\t(\d+)', out)
        if rc != 0 or not m:
            raise CtlError('Start-Process が失敗した (rc=%d): %s'
                           % (rc, (out + err).strip()[:300]))
        return int(m.group(1))


def parse_probe(win_paths, rc, out, err):
    """LOCK_SCRIPT の出力を解く。rc≠0・報告の欠け・error 行は ProbeError。"""
    errors = []
    result = {}
    lines = out.splitlines()
    for line in lines:
        parts = line.split('\t')
        if len(parts) < 2 or not parts[1].isdigit():
            continue
        idx = int(parts[1])
        if idx >= len(win_paths):
            continue
        if parts[0] in ('free', 'locked', 'missing'):
            result[win_paths[idx]] = parts[0]
        elif parts[0] == 'error':
            errors.append('%s (%s)' % (win_paths[idx],
                                       parts[2] if len(parts) > 2 else '?'))
    lacking = [p for p in win_paths if p not in result and
               not any(e.startswith(p + ' (') for e in errors)]
    detail = ('\n  stderr: ' + err.strip()[:300]) if err.strip() else ''
    if rc != 0:
        raise ProbeError('ロックの検査が失敗した (PowerShell rc=%d)%s' % (rc, detail))
    if errors:
        raise ProbeError('ロックの検査が失敗した (ロックではない例外):\n  %s%s'
                         % ('\n  '.join(errors), detail))
    if lacking or 'end' not in lines:
        raise ProbeError('ロックの検査が結果を返さなかった: %s%s'
                         % (', '.join(lacking) or '(終端なし)', detail))
    return result


class Http(object):
    """aidebug への HTTP。WSL から直に届かなければ Windows の curl.exe を使う
    (tools/np21w_mcp/np21w_client.py と同じ逃げ道)。届かなければ None を返す。"""

    def __init__(self, base=None):
        self.base = base or AIDEBUG_URL

    def request(self, method, path, body=None, timeout=10):
        """(status, text) か None (届かない)。"""
        data = body.encode('utf-8') if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode('utf-8', 'replace')
        except (urllib.error.URLError, OSError):
            pass
        args = [CURL, '-s', '-m', str(max(1, int(timeout))), '-w', '\n%{http_code}',
                '-X', method, self.base + path]
        if body is not None:
            args += ['--data-binary', body]
        try:
            p = subprocess.run(args, capture_output=True, timeout=timeout + 5,
                               cwd='/mnt/c' if os.path.isdir('/mnt/c') else None)
        except (OSError, subprocess.SubprocessError):
            return None
        if p.returncode != 0:
            return None
        text = p.stdout.decode('utf-8', 'replace')
        head, _, code = text.rpartition('\n')
        if not code.strip().isdigit() or code.strip() == '000':
            return None
        return int(code.strip()), head


# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------
def format_dialog(js):
    """/api/dialog の JSON を 1 段落に。"""
    parts = []
    for d in js.get('dialogs') or []:
        if not d.get('modal', True):
            continue
        s = '[%s]' % (d.get('title') or '')
        text = ' / '.join(t for t in d.get('text') or [] if t)
        if text:
            s += ' ' + text
        buttons = ', '.join(b.get('text') or str(b.get('id')) for b in d.get('buttons') or [])
        if buttons:
            s += ' (ボタン: %s)' % buttons
        parts.append(s)
    return '\n  '.join(parts) or '(本文を読めなかった)'


class Ctl(object):
    def __init__(self, paths, ops=None, http=None, sleep=time.sleep,
                 clock=time.monotonic, out=None, err=None, poll=1.0, settle=2.0):
        self.paths = paths
        self.ops = ops or WinOps()
        self.http = http or Http()
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

    # -- HTTP ------------------------------------------------------------------
    def api(self, method, path, body=None, timeout=10):
        """(status, dict|None)。届かなければ (None, None)。"""
        r = self.http.request(method, path, body, timeout)
        if r is None:
            return None, None
        status, text = r
        try:
            js = json.loads(text)
        except ValueError:
            js = None
        return status, js if isinstance(js, dict) else None

    def instance(self):
        """('ok', js) / ('old', None) / ('down', None) / ('bad', status)"""
        st, js = self.api('GET', '/api/instance', timeout=5)
        if st is None:
            return 'down', None
        if st == 200 and js and js.get('instance_id') and js.get('pid'):
            return 'ok', js
        if st == 404:
            return 'old', None
        return 'bad', st

    def dialog(self):
        """モーダルのダイアログが出ていればその JSON、無ければ None。"""
        st, js = self.api('GET', '/api/dialog', timeout=5)
        if st == 200 and js and js.get('open') and js.get('modal', True):
            return js
        return None

    def fail_if_dialog(self, context):
        js = self.dialog()
        if js:
            raise CtlError('%s — NP21/W がダイアログを出している:\n  %s'
                           % (context, format_dialog(js)))

    # -- プロセス --------------------------------------------------------------
    def expected_exe(self, exe):
        return self.paths.win_of(exe)

    def split_procs(self, exe):
        """(この exe のもの, それ以外)。exe が読めないものは「それ以外」。"""
        want = win_norm(self.expected_exe(exe))
        ours, others = [], []
        for p in self.ops.list_processes():
            (ours if p.exe and win_norm(p.exe) == want else others).append(p)
        return ours, others

    def _until(self, deadline, done):
        """done() が真になるか期限まで poll 間隔で呼ぶ。最後の値を返す。"""
        while True:
            value = done()
            if value or self.clock() >= deadline:
                return value
            self.sleep(self.poll)

    def note_others(self, others):
        for p in others:
            self.say('対象外: %s pid=%d exe=%s' % (p.name, p.pid, p.exe or '(読めない)'))

    def wait_ours_gone(self, exe, deadline, pids=None):
        def gone():
            ours, _ = self.split_procs(exe)
            return not [p for p in ours if pids is None or p.pid in pids]
        if self._until(deadline, gone):
            return
        ours, _ = self.split_procs(exe)
        raise CtlError('NP21/W (%s) のプロセスが期限までに消えない: %s'
                       % (self.expected_exe(exe),
                          ', '.join('pid=%d' % p.pid for p in ours)))

    def force_kill(self, exe, deadline):
        ours, others = self.split_procs(exe)
        self.note_others(others)
        if not ours:
            self.say('stopped (この exe のプロセスは無い)')
            return 0
        for p in ours:
            self.say('taskkill /F %s pid=%d' % (p.exe, p.pid))
            self.ops.kill(p.pid)
        self.wait_ours_gone(exe, deadline)
        self.say('stopped (強制終了)')
        return 0

    def stop(self, exe=DEFAULT_EXE, timeout=60):
        check_name(exe, '--exe')
        deadline = self.clock() + timeout
        want = win_norm(self.expected_exe(exe))
        state, inst = self.instance()
        if state == 'ok' and win_norm(inst.get('exe')) != want:
            self.warn('aidebug に答えているのは別の場所の NP21/W (%s) — API では止めない'
                      % inst.get('exe'))
        elif state == 'ok':
            pid = int(inst['pid'])
            body = urllib.parse.urlencode({'save': '0',
                                           'instance_id': inst['instance_id']})
            st, js = self.api('POST', '/api/quit', body, timeout=10)
            if st == 200:
                self.say('quit 要求 (save=0) pid=%d instance=%s'
                         % (pid, inst['instance_id']))
                try:
                    self.wait_ours_gone(exe, deadline, pids={pid})
                    self.say('stopped (/api/quit)')
                    ours, others = self.split_procs(exe)
                    self.note_others(others)
                    if ours:
                        return self.force_kill(exe, deadline)
                    return 0
                except CtlError:
                    self.warn('quit を受けたが pid=%d が期限までに消えない — 強制終了に落とす'
                              % pid)
            else:
                self.warn('/api/quit が失敗した (HTTP %s %s) — 強制終了に落とす'
                          % (st, (js or {}).get('error', '')))
        elif state == 'old':
            self.warn(OLD_FORK + ' — 強制終了に落とす (exe パスが一致するものだけ)')
        elif state == 'down':
            self.say('aidebug が応答しない — 強制終了に落とす (exe パスが一致するものだけ)')
        else:
            self.warn('/api/instance が想定外の応答 (HTTP %s) — 強制終了に落とす' % inst)
        return self.force_kill(exe, max(deadline, self.clock() + 10))

    # -- 媒体のロック ----------------------------------------------------------
    def _probe(self, paths, deadline):
        remaining = deadline - self.clock()
        if remaining <= 0:
            return None
        return self.ops.probe(paths, remaining)

    def wait_media(self, targets, deadline, stable=5.0):
        """各媒体が**続けて stable 秒**開けることを、起動の直前まで確かめる。

        - 安定した媒体も毎回プローブし続け、1 回でもロックされたら数え直す
          (nhd-pull の直後に Windows 側が掴み直した例がある、§4-60)
        - 全部安定したら settle 秒置いて、**全部をもう一度**プローブしてから返す
        - 期限を過ぎてから得た結果は成功に数えない
        """
        required = dict(targets)
        order = [p for p, _ in targets]
        state = {}
        free_since = {}
        last_free = {}
        missing = set()

        def settled(p):
            return p in missing or (
                p in free_since and last_free[p] - free_since[p] >= stable)

        def apply(result, now, final=False):
            relocked = []
            for p, st in result.items():
                state[p] = st
                if st == 'missing':
                    if required[p]:
                        raise CtlError('媒体が無い: %s' % p, 2)
                    if p not in missing:
                        self.warn('ini にある媒体が無い (待たない): %s' % p)
                    missing.add(p)
                elif st == 'free':
                    free_since.setdefault(p, now)
                    last_free[p] = now
                else:
                    if p in free_since:
                        relocked.append(p)
                    free_since.pop(p, None)
                    last_free.pop(p, None)
            for p in relocked:
                self.say('locked again%s (数え直し): %s'
                         % (' 起動の直前' if final else '', p))

        def timeout_error():
            locked = [p for p in order if state.get(p) == 'locked']
            unsteady = [p for p in order
                        if p not in missing and p not in locked and not settled(p)]
            msg = '媒体が期限までに %g 秒続けて開けない:' % stable
            for p in locked:
                msg += '\n  locked: ' + p
            for p in unsteady:
                msg += '\n  不安定 (開けたり掴まれたり): ' + p
            if not locked and not unsteady:
                msg += '\n  (検査が期限を過ぎた)'
            return CtlError(msg)

        while True:
            probe_list = [p for p in order if p not in missing]
            result = self._probe(probe_list, deadline)
            if result is None or self.clock() > deadline:
                raise timeout_error()
            apply(result, self.clock())
            if all(settled(p) for p in order):
                # 起動の直前: 置いてから全部をもう一度
                if self.settle:
                    self.sleep(self.settle)
                probe_list = [p for p in order if p not in missing]
                result = self._probe(probe_list, deadline)
                if result is None or self.clock() > deadline:
                    raise timeout_error()
                apply(result, self.clock(), final=True)
                if all(state.get(p) in ('free', 'missing') for p in order):
                    for p in order:
                        if p not in missing:
                            self.say('free %gs: %s' % (stable, p))
                    return
            if self.clock() >= deadline:
                raise timeout_error()
            self.sleep(self.poll)

    # -- 起動 ------------------------------------------------------------------
    def start(self, ini, fd=None, exe=DEFAULT_EXE, timeout=60,
              wait_ready=False, ready_timeout=180, stable=5.0,
              alive=10.0, api_timeout=60.0):
        check_name(ini, '--ini')
        check_name(exe, '--exe')
        if fd:
            check_name(fd, '--fd')
        if timeout < stable:
            raise CtlError('--timeout (%g) は --stable (%g) 以上にする' % (timeout, stable), 2)
        if alive < 0 or api_timeout < 0:
            raise CtlError('--alive / --api-timeout は 0 以上', 2)
        if not os.path.isfile(self.paths.wsl_of(exe)):
            raise CtlError('exe が無い: %s' % self.paths.wsl_of(exe), 2)
        if not os.path.isfile(self.paths.wsl_of(ini)):
            raise CtlError('ini が無い: %s' % self.paths.wsl_of(ini), 2)

        deadline = self.clock() + timeout
        ours, others = self.split_procs(exe)
        self.note_others(others)
        if ours:
            self.say('この exe のプロセスが残っている — 消えるのを待つ: %s'
                     % ', '.join('pid=%d' % p.pid for p in ours))
            self.wait_ours_gone(exe, deadline)
        targets, _source = media_targets(self.paths, ini, fd, self.warn)
        self.wait_media(targets, deadline, stable)
        args = ['/i' + self.paths.win_of(ini)]
        if fd:
            args.append(self.paths.win_of(fd))
        pid = self.ops.start(self.expected_exe(exe), args, self.paths.win)
        self.say('started %s pid=%d (%s)' % (exe, pid, ' '.join(args)))
        self.verify_started(pid, exe, ini, alive, api_timeout)
        if wait_ready:
            return self.wait_ready(ready_timeout)
        return 0

    def verify_started(self, pid, exe, ini, alive=10.0, api_timeout=60.0):
        """起動したプロセスが alive 秒生き、/api/instance が pid と exe の一致を
        返すまで見る。途中で消えたら「起動直後に終了した」。API が応答しない・
        進まないときはダイアログを確かめる。--api-timeout 0 は API を待たない。"""
        begin = self.clock()
        api_deadline = begin + api_timeout
        want_exe = win_norm(self.expected_exe(exe))
        want_ini = win_norm(self.paths.win_of(ini))
        confirmed = api_timeout <= 0
        old_warned = False

        def alive_now():
            ours, _ = self.split_procs(exe)
            return pid in [p.pid for p in ours]

        while True:
            if not alive_now():
                raise CtlError('起動直後に終了した (pid=%d、起動から %.0f 秒)。'
                               '媒体が開けなかった可能性 — status で媒体を確かめる'
                               % (pid, self.clock() - begin))
            if not confirmed:
                state, inst = self.instance()
                if state == 'ok':
                    if int(inst['pid']) != pid or win_norm(inst.get('exe')) != want_exe:
                        raise CtlError(
                            'aidebug に答えているのは起動したプロセスではない: '
                            'API pid=%s exe=%s / 起動 pid=%d exe=%s'
                            % (inst.get('pid'), inst.get('exe'), pid,
                               self.expected_exe(exe)))
                    if inst.get('ini') and win_norm(inst['ini']) != want_ini:
                        self.warn('NP21/W が読んだ ini が指定と違う: %s' % inst['ini'])
                    if inst.get('dialog'):
                        self.fail_if_dialog('起動の確認中')
                    confirmed = True
                    self.say('instance pid=%d id=%s ini=%s'
                             % (pid, inst['instance_id'], inst.get('ini')))
                elif state == 'old':
                    if not old_warned:
                        self.warn(OLD_FORK + ' — pid と exe の照合はできない')
                        old_warned = True
                    st, js = self.api('GET', '/api/status', timeout=5)
                    if st == 200:
                        confirmed = True
                else:
                    # API が黙っている: ダイアログで止まっていないか
                    self.fail_if_dialog('起動の確認中 (aidebug が応答しない)')
            else:
                self.fail_if_dialog('起動の確認中')
            if confirmed and self.clock() - begin >= alive:
                # 成功を返す直前にもう一度
                if not alive_now():
                    raise CtlError('起動直後に終了した (pid=%d、確認の最後に消えた)' % pid)
                self.say('alive %gs pid=%d%s' % (alive, pid,
                         ', aidebug up' if api_timeout > 0 else ' (API は待たない)'))
                return
            if not confirmed and self.clock() >= api_deadline:
                self.fail_if_dialog('起動の確認中')
                raise CtlError('pid=%d は生きているが aidebug が %g 秒応答しない '
                               '(ini の aidebug=true / aidbport)' % (pid, api_timeout))
            self.sleep(self.poll)

    # -- 起動完了 --------------------------------------------------------------
    def wait_ready(self, timeout=180):
        deadline = self.clock() + timeout
        seen = {'text': None}

        def ready():
            r = self.http.request('GET', '/api/tvram', None, 15)
            if r is None:
                return False
            st, text = r
            if st == 503:
                self.fail_if_dialog('起動完了待ち')
                return False
            if st != 200:
                return False
            seen['text'] = text
            return READY_TEXT in text

        if self._until(deadline, ready):
            self.say('ready (%s)' % READY_TEXT)
            return 0
        self.fail_if_dialog('起動完了待ち')
        if seen['text'] is None:
            raise CtlError('%d 秒たっても aidebug API に届かない '
                           '(NP21/W が動いているか、ini の aidebug=true / aidbport)'
                           % timeout)
        tail = [l for l in seen['text'].splitlines() if l.strip()][-5:]
        raise CtlError('%d 秒たっても "%s" が出ない。画面の最後:\n  %s'
                       % (timeout, READY_TEXT, '\n  '.join(tail)))

    # -- FD --------------------------------------------------------------------
    def fdd(self, drive, insert=None, eject=False, readonly=False, exe=DEFAULT_EXE):
        if drive not in (1, 2, 3, 4):
            raise CtlError('--drive は 1〜4', 2)
        if bool(insert) == bool(eject):
            raise CtlError('--insert <name> か --eject のどちらか 1 つ', 2)
        params = {'drive': str(drive)}
        if insert:
            check_name(insert, '--insert')
            if not os.path.isfile(self.paths.wsl_of(insert)):
                raise CtlError('イメージが無い: %s' % self.paths.wsl_of(insert), 2)
            params.update({'action': 'insert', 'path': self.paths.win_of(insert),
                           'readonly': '1' if readonly else '0'})
        else:
            params['action'] = 'eject'
        state, inst = self.instance()
        if state == 'old':
            raise CtlError(OLD_FORK)
        if state != 'ok':
            raise CtlError('aidebug が応答しない (NP21/W が動いているか)')
        if win_norm(inst.get('exe')) != win_norm(self.expected_exe(exe)):
            raise CtlError('aidebug に答えているのは別の場所の NP21/W (%s)' % inst.get('exe'))
        st, js = self.api('POST', '/api/fdd', urllib.parse.urlencode(params), timeout=15)
        if st == 200:
            self.say('fdd%d %s%s' % (drive, params['action'],
                                     (' ' + params['path']) if insert else ''))
            return 0
        if st == 404 and (js or {}).get('error') == 'unknown endpoint':
            raise CtlError(OLD_FORK)
        raise CtlError('/api/fdd が失敗した (HTTP %s): %s'
                       % (st, (js or {}).get('error', '応答なし')))

    # -- 状態 ------------------------------------------------------------------
    def status(self, ini=None, exe=DEFAULT_EXE):
        check_name(exe, '--exe')
        if ini:
            check_name(ini, '--ini')
        ours, others = self.split_procs(exe)
        for p in ours:
            self.say('process: pid=%d %s' % (p.pid, p.exe))
        if not ours:
            self.say('process: none (%s)' % self.expected_exe(exe))
        self.note_others(others)
        state, inst = self.instance()
        if state == 'ok':
            self.say('aidebug: up pid=%s id=%s started=%s'
                     % (inst.get('pid'), inst.get('instance_id'), inst.get('started_at')))
            self.say('  exe: %s' % inst.get('exe'))
            self.say('  ini: %s' % inst.get('ini'))
            for f in inst.get('fdd') or []:
                if f.get('path'):
                    self.say('  fdd%s: %s' % (f.get('drive'), f.get('path')))
            for s in inst.get('ide') or []:
                if s.get('path'):
                    self.say('  ide%s (%s): %s' % (s.get('slot'), s.get('type'), s.get('path')))
        elif state == 'old':
            self.say('aidebug: up (古いフォーク — /api/instance が無い)')
        elif state == 'down':
            self.say('aidebug: down')
        else:
            self.say('aidebug: 想定外の応答 (HTTP %s)' % inst)
        if state in ('ok', 'bad'):
            st, js = self.api('GET', '/api/dialog', timeout=5)
            if st == 200 and js is not None:
                if js.get('open'):
                    self.say('dialog: open%s\n  %s' % (' (modal)' if js.get('modal') else '',
                                                        format_dialog(js)))
                else:
                    self.say('dialog: none')
        targets, source = media_targets(self.paths, ini, None, self.warn)
        self.say('媒体 (%s):' % source)
        state_map = self.ops.probe([p for p, _ in targets], 60)
        for p, _ in targets:
            self.say('  %-7s %s' % (state_map.get(p, '?') + ':', p))
        return 0


def build_parser():
    ap = argparse.ArgumentParser(
        prog='np21w_ctl.py',
        description='NP21/W の停止 (/api/quit)・起動 (媒体のロック解除を待つ)・'
                    '起動完了待ち・FD の出し入れ・状態')
    sub = ap.add_subparsers(dest='cmd')
    sub.required = True
    p = sub.add_parser('stop', help='/api/quit で止める。API が無ければ exe 一致のものだけ強制終了')
    p.add_argument('--exe', default=DEFAULT_EXE)
    p.add_argument('--timeout', type=float, default=60)
    p = sub.add_parser('start', help='プロセス 0・媒体の解放を確かめてから起動する')
    p.add_argument('--ini', required=True, help='NP21W_DIR 直下の ini の名前')
    p.add_argument('--fd', help='FD 引数で渡すイメージの名前')
    p.add_argument('--exe', default=DEFAULT_EXE)
    p.add_argument('--timeout', type=float, default=60,
                   help='起動までの全体の上限秒 (既定 60、--stable 以上)')
    p.add_argument('--stable', type=float, default=5,
                   help='各媒体が続けて開けることを確かめる秒数 (既定 5、1 秒おき)')
    p.add_argument('--alive', type=float, default=10,
                   help='起動後にプロセスが生きていることを見る秒数 (既定 10)')
    p.add_argument('--api-timeout', type=float, default=60,
                   help='起動後に /api/instance の応答を待つ上限秒 (既定 60、0 = 待たない)')
    p.add_argument('--wait-ready', action='store_true',
                   help='起動後に wait-ready まで続ける')
    p.add_argument('--ready-timeout', type=float, default=180)
    p = sub.add_parser('wait-ready', help='/api/tvram に "%s" が出るまで待つ' % READY_TEXT)
    p.add_argument('--timeout', type=float, default=180)
    p = sub.add_parser('fdd', help='FD の出し入れ (/api/fdd、ini は変えない)')
    p.add_argument('--drive', type=int, required=True)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--insert', help='NP21W_DIR 直下のイメージの名前')
    g.add_argument('--eject', action='store_true')
    p.add_argument('--readonly', action='store_true')
    p.add_argument('--exe', default=DEFAULT_EXE)
    p = sub.add_parser('status', help='プロセス・aidebug・ダイアログ・媒体のロック状態')
    p.add_argument('--ini', default=None, help='媒体を拾う ini (既定: 既定の 3 つ)')
    p.add_argument('--exe', default=DEFAULT_EXE)
    return ap


def main(argv=None, ctl_factory=None):
    args = build_parser().parse_args(argv)
    ctl = (ctl_factory or (lambda: Ctl(paths_from_env())))()
    try:
        if args.cmd == 'stop':
            return ctl.stop(args.exe, args.timeout)
        if args.cmd == 'start':
            return ctl.start(args.ini, args.fd, args.exe, args.timeout,
                             args.wait_ready, args.ready_timeout, args.stable,
                             args.alive, args.api_timeout)
        if args.cmd == 'wait-ready':
            return ctl.wait_ready(args.timeout)
        if args.cmd == 'fdd':
            return ctl.fdd(args.drive, args.insert, args.eject, args.readonly, args.exe)
        return ctl.status(args.ini, args.exe)
    except CtlError as exc:
        ctl.warn(str(exc))
        return exc.code


if __name__ == '__main__':
    sys.exit(main())
