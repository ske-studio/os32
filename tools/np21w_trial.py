#!/usr/bin/env python3
"""One approved Cirrus trial. Default dry-run does not open files or processes.

The trusted host selects PID/creation/exe, baseline and cwd. A local LLM must
propose exactly the immutable one-step plan before the executor is constructed.
Normal close may save settings; baseline is NOT proof of previous active ini.
"""
import argparse
import copy
import json
import ntpath
import re
import sys
import uuid
from types import FunctionType
import np21w_ini_live as live
from np21w_ini import IniError, LIMIT, SECTION, transform
from emu_agent.playbook import strict_object, exact_equal


def transform_trial(raw):
    candidate, diff = transform(raw, {'USEGD5430': 'true', 'GD5430TYPE': '91'})
    # ini.cpp:848 PFTYPE_BOOL e_resume -> np2oscfg.resume; np2.cpp:4655
    # loads saved VM state when true. Require a known explicit value.
    parts = re.split(b'(\r\n|\r|\n)', candidate)
    section, found = None, []
    for i in range(0, len(parts), 2):
        line = parts[i]
        body = line[3:] if i == 0 and line.startswith(b'\xef\xbb\xbf') else line
        stripped = body.strip(b' \t')
        if stripped.startswith(b'['):
            section = stripped.split(b']', 1)[0][1:].strip().lower()
        elif section == SECTION:
            match = re.fullmatch(rb'([ \t]*e_resume[ \t]*=[ \t]*)([^;#]*)([;#].*)?', body, re.I)
            if match:
                token = match[2].rstrip(b' \t')
                if token not in (b'true', b'false'):
                    raise IniError('unknown resume value')
                found.append((i, len(line) - len(body) + match.start(2), token))
    if len(found) != 1:
        raise IniError('missing or duplicate e_resume')
    i, start, token = found[0]
    if token == b'true':
        parts[i] = parts[i][:start] + b'false' + parts[i][start + len(token):]
        diff.append('e_resume: true -> false')
    candidate = b''.join(parts)
    if len(candidate) > LIMIT:
        raise IniError('trial exceeds snapshot limit')
    return candidate, diff


def _identity(row):
    if (type(row) is not dict or set(row) != {'pid', 'created', 'exe', 'command'} or
            type(row['pid']) is not int or not 0 < row['pid'] <= live.PROCESS_ID_MAX or
            type(row['created']) is not str or not re.fullmatch(
                r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{7}Z', row['created']) or
            type(row['command']) is not str or not row['command'] or len(row['command']) > 32767):
        raise IniError('invalid process identity')
    live.path_key(row['exe'])
    return row


def make_plan(*, exe, baseline, cwd, pid, created):
    for path in (exe, baseline, cwd):
        live.path_key(path)
    _identity(dict(pid=pid, created=created, exe=exe, command='operator selected'))
    if (ntpath.basename(exe).lower() != 'np21x64w.exe' or
            live.path_key(baseline) != live.path_key(ntpath.splitext(exe)[0] + '.ini') or
            live.path_key(cwd) != live.path_key(ntpath.dirname(exe))):
        raise IniError('require exe-adjacent baseline and explicit exe-directory cwd')
    trial = ntpath.join(cwd, 'np21w-trial-' + uuid.uuid4().hex + '.ini')
    live.path_key(trial)
    return dict(action='cirrus-trial', exe=exe, baseline=baseline, cwd=cwd,
                pid=pid, created=created, trial=trial,
                changes={'USEGD5430': 'true', 'GD5430TYPE': '91', 'e_resume': 'false'},
                lifecycle={'close': 'normal-only', 'baseline_read': 'after-verified-exit',
                           'baseline_role': 'operator-chosen-not-active-config-proof',
                           'normal_exit_may_save': True, 'exact_vm_ram_preserved': False,
                           'new_ini_and_cwd': 'explicit', 'retry': False, 'restore': False})


def _validate_plan(plan):
    try:
        expected = make_plan(**{k: plan[k] for k in ('exe', 'baseline', 'cwd', 'pid', 'created')})
        if (type(plan['trial']) is not str or
                not re.fullmatch(r'np21w-trial-[a-f0-9]{32}\.ini', ntpath.basename(plan['trial'])) or
                ntpath.dirname(plan['trial']) != plan['cwd']):
            raise IniError('invalid unique trial path')
        live.path_key(plan['trial'])
        expected['trial'] = plan['trial']
        if not exact_equal(plan, expected):
            raise IniError('invalid bounded trial plan')
    except (KeyError, TypeError, AttributeError) as exc:
        raise IniError('invalid bounded trial plan') from exc


def bind_trial(plan, executor_factory, *, authorized=False, exclusive=False):
    """Expose only this single-use callable to the local model; never executor.

    JSON serialization freezes every nested approval field. Models only supply
    strict JSON text; no paths or settings are taken from the returned proposal.
    A rejected proposal also consumes this binding. No side effects precede gate.
    """
    _validate_plan(plan)
    frozen = json.dumps(plan, sort_keys=True)
    used = False
    def dispatch(proposal):
        nonlocal used
        if used:
            raise IniError('trial approval already consumed')
        used = True
        if authorized is not True or exclusive is not True:
            raise IniError('exact trial approval and exclusive operator required')
        approved = json.loads(frozen)
        try:
            if not exact_equal(strict_object(proposal), approved):
                raise ValueError()
        except (ValueError, TypeError, RecursionError):
            raise IniError('local proposal does not exactly match approval') from None
        return _run(approved, executor_factory)
    return dispatch


def _run(plan, factory):
    result = dict(ok=False, stage='executor', trial=plan['trial'], cwd=plan['cwd'],
                  completed=[], retry=False, restore=False, plan=copy.deepcopy(plan))
    try:
        with factory(copy.deepcopy(plan)) as ex:
            def call(op, **args):
                result['stage'] = op
                value = ex.call(op, **args)
                result['completed'].append(op)
                return value
            def rows():
                value = call('query')
                if type(value) is not list:
                    raise IniError('invalid query')
                for row in value:
                    _identity(row)
                return value
            def absent():
                if rows():
                    raise IniError('emulator remains present')
            call('lock')
            call('preflight')
            current = rows()
            if (len(current) != 1 or any(current[0][k] != plan[k] for k in ('pid', 'created')) or
                    live.path_key(current[0]['exe']) != live.path_key(plan['exe'])):
                raise IniError('selected identity mismatch')
            old = current[0]
            if rows() != [old]:
                raise IniError('identity changed before close')
            call('close', process=old)
            absent()
            before = live.checked_snapshot(call('snapshot'))
            result['stage'] = 'transform'
            candidate, diff = transform_trial(before['data'])
            result['diff'] = diff
            absent()
            call('create', expected=before, data=candidate)
            absent()
            call('verify', expected=before, data=candidate)
            started = _identity(call('start', expected=before, data=candidate))
            command = '"' + plan['exe'] + '" "' + plan['trial'] + '"'
            if (started['created'] == old['created'] or started['exe'] != plan['exe'] or
                    started['command'] != command):
                raise IniError('new process identity mismatch')
            if rows() != [started] or rows() != [started]:
                raise IniError('new process identity unstable')
            result.update(process=started, stage='dispose')
        result.update(ok=True, stage='verified')
    except Exception:
        # No raw transport/model/ini text; path and completed stages are evidence.
        result['reason'] = 'trial failed; inspect recorded stage and process state; no automatic recovery'
    return result


# Reuse only fixed read-only/path/CreateNew helpers, never live stop/replace.
PS_SERVER = live.PS_SERVER.split('function Bundle($id) {', 1)[0] + r'''
function VerifyTrial($a) {
 AssertAbsent
 AssertSnapshot $a.expected
 if ((Snapshot $plan.trial).data -cne $a.data) { throw 'trial changed' }
}
try {
 while ($null -ne ($line = [Console]::ReadLine())) {
  try {
   $request = $line | ConvertFrom-Json -ErrorAction Stop
   $serialized = $request.target | ConvertTo-Json -Depth 20 -Compress
   if (!$plan) {
    $plan = $request.target
    $bound = $serialized
    $target = @{exe=$plan.exe; ini=$plan.baseline}
   }
   if ($serialized -cne $bound) { throw 'plan changed' }
   $a = $request.args
   $value = $true
   if ($request.op -ne 'lock' -and !$locked) { throw 'lock required' }
   switch ($request.op) {
    'lock' {
     if ($locked) { throw 'already locked' }
     $mutex = [System.Threading.Mutex]::new($false, 'Global\OS32.NP21W.Ini.Live')
     if (!$mutex.WaitOne(0)) { throw 'workflow busy' }
     $locked = $true
    }
    'preflight' {
     CheckPath $plan.exe
     CheckPath $plan.cwd
     $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($plan.exe))
     if ($drive.DriveType -ne [IO.DriveType]::Fixed -or $drive.DriveFormat -ne 'NTFS') { throw 'local NTFS required' }
     if (Test-Path -LiteralPath $plan.trial) { throw 'trial collision' }
    }
    'query' { $value = @(Query) }
    'close' {
     $p = [Diagnostics.Process]::GetProcessById([int]$a.process.pid)
     try {
      $handle = $p.Handle
      AssertProcess $a.process
      if (!$p.CloseMainWindow()) { throw 'normal close refused' }
      if (!$p.WaitForExit(10000)) { throw 'normal exit timeout' }
      AssertAbsent
     } finally { $p.Dispose() }
    }
    'snapshot' { AssertAbsent; $value = Snapshot $plan.baseline }
    'create' {
     AssertAbsent
     AssertSnapshot $a.expected
     NewFile $plan.trial ([Convert]::FromBase64String($a.data))
     VerifyTrial $a
    }
    'verify' { VerifyTrial $a }
    'start' {
     VerifyTrial $a
     CheckPath $plan.exe
     CheckPath $plan.cwd
     $si = [Diagnostics.ProcessStartInfo]::new()
     $si.UseShellExecute = $false
     $si.FileName = $plan.exe
     $si.Arguments = '"' + $plan.trial + '"'
     $si.WorkingDirectory = $plan.cwd
     $p = [Diagnostics.Process]::Start($si)
     try {
      $handle = $p.Handle
      if ($p.WaitForExit(1000)) { throw 'started process exited' }
      $rows = @(Query)
      if ($rows.Count -ne 1 -or $rows[0].pid -ne $p.Id -or
          $rows[0].created -cne $p.StartTime.ToUniversalTime().ToString('o') -or
          $rows[0].exe -cne $plan.exe -or
          $rows[0].command -cne ('"' + $plan.exe + '" "' + $plan.trial + '"')) {
       throw 'new identity mismatch'
      }
      $value = $rows[0]
     } finally { $p.Dispose() }
    }
    default { throw 'unknown trial operation' }
   }
   @{ok=$true; value=$value} | ConvertTo-Json -Depth 24 -Compress | ForEach-Object { [Console]::WriteLine($_) }
  } catch {
   [Console]::WriteLine('{"ok":false}')
   break
  }
 }
} finally {
 if ($locked) { $mutex.ReleaseMutex() }
 if ($mutex) { $mutex.Dispose() }
}
'''


class PowerShellTransport(live.PowerShellTransport):
    def __init__(self):
        namespace = dict(vars(live), PS_SERVER=PS_SERVER)
        FunctionType(live.PowerShellTransport.__init__.__code__, namespace)(self)

    def close(self):
        # Even the transport has no kill fallback. Closing stdin lets its fixed
        # finally release the mutex. A timeout is reported, never retried.
        self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        finally:
            self._close_reader(failed=sys.exc_info()[0] is not None)


class WindowsExecutor:
    """Private trusted adapter; fixed trial operations, no generic actions."""
    OPS = {'lock', 'preflight', 'query', 'close', 'snapshot', 'create', 'verify', 'start'}
    def __init__(self, plan, transport=None):
        _validate_plan(plan)
        self.plan = copy.deepcopy(plan)
        self.transport = transport
    def __enter__(self):
        if self.transport is None:
            self.transport = PowerShellTransport()
        return self
    def __exit__(self, *args):
        self.transport.close()
    def call(self, op, **args):
        if op not in self.OPS:
            raise IniError('unknown trial operation')
        try:
            response = self.transport.exchange(dict(op=op, target=self.plan, args=live.wire(args)))
            if (type(response) is not dict or set(response) != {'ok', 'value'} or response['ok'] is not True):
                raise IniError('trial executor rejected operation')
            value = live.wire(response['value'], decode=True)
            if op == 'snapshot':
                live.checked_snapshot(value)
            elif op == 'start':
                _identity(value)
            elif op == 'query':
                if type(value) is not list:
                    raise IniError('invalid query')
                for row in value:
                    _identity(row)
            elif value is not True:
                raise IniError('unconfirmed trial operation')
            return value
        except (ValueError, TypeError, KeyError, AttributeError):
            raise IniError('invalid trial executor response') from None


def local_proposal(plan, url, model):
    """One loopback OpenAI-compatible request; no env, auth, retries or tools."""
    import http.client
    import urllib.parse
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', '::1', 'localhost') or
            parsed.username or parsed.password or parsed.query or parsed.fragment or
            parsed.path != '/v1/chat/completions'):
        raise IniError('explicit loopback chat-completions URL required')
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=60)
    try:
        body = json.dumps(dict(model=model, stream=False, messages=[
            dict(role='system', content='Return exactly the approved one-step JSON object, without tools or extra text.'),
            dict(role='user', content=json.dumps(plan))]))
        connection.request('POST', parsed.path, body=body, headers={'Content-Type': 'application/json'})
        response = connection.getresponse()
        raw = response.read(65537)
        if response.status != 200 or len(raw) > 65536:
            raise IniError('local proposal unavailable')
        value = strict_object(raw.decode('utf-8'))
        choices = value['choices']
        if type(choices) is not list or len(choices) != 1:
            raise IniError('one local proposal required')
        message = choices[0]['message']
        if message.get('tool_calls') is not None or message.get('function_call') is not None:
            raise IniError('local proposal tools forbidden')
        return message['content']
    except Exception:
        raise IniError('local proposal failed') from None
    finally:
        connection.close()


def main(argv=None, executor_factory=WindowsExecutor, llm=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    for key in ('exe', 'baseline', 'cwd', 'created'):
        parser.add_argument('--' + key, required=True)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--exclusive-operator', action='store_true')
    parser.add_argument('--llm-url', default='http://127.0.0.1:1234/v1/chat/completions')
    parser.add_argument('--model', default='local-model', help='operator-selected local model ID')
    args = parser.parse_args(argv)
    try:
        plan = make_plan(**{k: getattr(args, k) for k in ('exe', 'baseline', 'cwd', 'pid', 'created')})
        if not args.execute:
            print(json.dumps(dict(mode='dry_run', executed=False, plan=plan)))
            return 0
        if not args.exclusive_operator:
            raise IniError('exclusive operator prerequisite required')
        gate = bind_trial(plan, executor_factory, authorized=True, exclusive=True)
        # Freeze approval before giving a separate copy to the local LLM.
        proposal = (llm or local_proposal)(copy.deepcopy(plan), args.llm_url, args.model)
        result = gate(proposal)
        print(json.dumps(result))
        return 0 if result['ok'] else 2
    except Exception:
        print(json.dumps(dict(ok=False, stage='approval', reason='invalid setup or rejected local proposal; no lifecycle execution')))
        return 2


if __name__ == '__main__':
    sys.exit(main())
