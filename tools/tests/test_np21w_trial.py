"""Synthetic host integration only; never contact an emulator or real ini."""
import copy
import io
import json
from pathlib import Path
import sys
import unittest
from contextlib import redirect_stdout
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import np21w_trial as trial
from np21w_ini import IniError
from np21w_ini_live import wire

EXE = r'C:\Trial Fixture\np21x64w.exe'
BASE = r'C:\Trial Fixture\np21x64w.ini'
CWD = r'C:\Trial Fixture'
CREATED = '2026-09-09T01:02:03.0000000Z'
RAW = (b'\xef\xbb\xbf[NekoProject21]\r\nUSEGD5430 = false ;keep\nGD5430TYPE=91\r\n'
       b'USEPEGCP=false\r\ne_resume = true\r\nopaque=\x82\xa0\n')
POST_EXIT = RAW + b'; normal exit saved settings\r\n'

def plan():
    return trial.make_plan(exe=EXE, baseline=BASE, cwd=CWD, pid=42, created=CREATED)

class Transport:
    def __init__(self, bound, fail=None, stay=False, old_command=None):
        self.plan = bound
        self.calls = []
        self.fail = fail
        self.stay = stay
        self.files = {BASE: RAW}
        self.rows = [dict(pid=42, created=CREATED, exe=EXE,
                          command=old_command or '"' + EXE + '"')]
    def exchange(self, request):
        op, a = request['op'], wire(request['args'], decode=True)
        self.calls.append(op)
        if self.fail == len(self.calls):
            return {'ok': False}
        value = True
        if op == 'query':
            value = copy.deepcopy(self.rows)
        elif op == 'close':
            if not self.stay:
                self.rows = []
                self.files[BASE] = POST_EXIT
        elif op == 'snapshot':
            assert not self.rows, 'baseline read before exit'
            value = dict(data=self.files[BASE], signature='post-exit')
        elif op == 'create':
            assert not self.rows
            assert a['expected']['data'] == self.files[BASE]
            assert self.plan['trial'] not in self.files
            self.files[self.plan['trial']] = a['data']
        elif op == 'verify':
            assert self.files[BASE] == a['expected']['data']
            assert self.files[self.plan['trial']] == a['data']
        elif op == 'start':
            self.rows = [dict(pid=43, created='2026-09-09T01:03:03.0000000Z', exe=EXE,
                              command='"' + EXE + '" "' + self.plan['trial'] + '"')]
            value = copy.deepcopy(self.rows[0])
        return dict(ok=True, value=wire(value))
    def close(self):
        self.calls.append('dispose')

class TrialTests(unittest.TestCase):
    def run_bound(self, p=None, **kw):
        p = p or plan()
        transport = Transport(p, **kw)
        factory = lambda bound: trial.WindowsExecutor(bound, transport)
        gate = trial.bind_trial(p, factory, authorized=True, exclusive=True)
        return p, transport, gate

    def test_transform_preserves_opaque_bytes_and_only_proven_changes(self):
        actual, diff = trial.transform_trial(RAW)
        self.assertEqual(actual, RAW.replace(b'USEGD5430 = false', b'USEGD5430 = true')
                         .replace(b'e_resume = true', b'e_resume = false'))
        self.assertEqual(diff, ['USEGD5430: false -> true', 'e_resume: true -> false'])
        self.assertEqual(trial.transform_trial(actual), (actual, []))

    def test_unknown_missing_duplicate_resume_and_cirrus_rejected(self):
        for raw in [RAW.replace(b'e_resume = true', b''), RAW + b'e_resume=false\n',
                    RAW.replace(b'e_resume = true', b'e_resume = 1'),
                    RAW.replace(b'GD5430TYPE=91', b'GD5430TYPE=90'),
                    RAW.replace(b'true', b'unknown'), b'\xff\xfe' + RAW]:
            with self.subTest(raw=raw), self.assertRaises(IniError):
                trial.transform_trial(raw)

    def test_plan_rejects_unsafe_target_and_unknown_setup(self):
        for change in [dict(pid=True), dict(created='yesterday'), dict(exe=r'C:\x.exe'),
                       dict(baseline=r'C:\other.ini'), dict(cwd=r'C:\elsewhere'),
                       dict(baseline=r'C:\Trial Fixture\..\np21x64w.ini')]:
            args = dict(exe=EXE, baseline=BASE, cwd=CWD, pid=42, created=CREATED)
            args.update(change)
            with self.subTest(change=change), self.assertRaises(IniError):
                trial.make_plan(**args)
        self.assertNotEqual(plan()['trial'], plan()['trial'])

    def test_gate_mismatch_no_executor_construction_single_use(self):
        p = plan()
        calls = []
        for proposal in [json.dumps(dict(p, pid=43)), '{}', json.dumps(p) + ' trailing',
                         '{"action":"cirrus-trial","action":"cirrus-trial"}']:
            gate = trial.bind_trial(p, lambda bound: calls.append(bound), authorized=True, exclusive=True)
            with self.assertRaises(IniError):
                gate(proposal)
        self.assertEqual(calls, [])

    def test_approval_flags_required_before_executor_and_bound_changes_fixed(self):
        p = plan()
        def forbidden(bound):
            self.fail('unapproved executor construction')
        for flags in [dict(), dict(authorized=True), dict(exclusive=True),
                      dict(authorized=1, exclusive=True)]:
            gate = trial.bind_trial(p, forbidden, **flags)
            with self.assertRaises(IniError):
                gate(json.dumps(p))
        for changed in [dict(p, changes={'USEGD5430': 'false'}), dict(p, shell='anything'),
                        dict(p, trial=BASE)]:
            with self.assertRaises(IniError):
                trial.bind_trial(changed, forbidden, authorized=True, exclusive=True)

    def test_immutable_binding_and_consumed_failure(self):
        p, t, gate = self.run_bound(fail=1)
        approved = json.dumps(p)
        p['pid'] = 777
        result = gate(approved)
        self.assertFalse(result['ok'])
        self.assertEqual(t.calls, ['lock', 'dispose'])
        with self.assertRaises(IniError):
            gate(approved)

    def test_full_mock_transport_order_post_exit_bytes_original_never_written(self):
        p, t, gate = self.run_bound()
        result = gate(json.dumps(p))
        self.assertTrue(result['ok'])
        self.assertEqual(t.calls, ['lock', 'preflight', 'query', 'query', 'close', 'query',
                                   'snapshot', 'query', 'create', 'query', 'verify', 'start',
                                   'query', 'query', 'dispose'])
        self.assertEqual(t.files[BASE], POST_EXIT)
        self.assertEqual(t.files[p['trial']], trial.transform_trial(POST_EXIT)[0])
        self.assertEqual(result['process']['pid'], 43)
        self.assertEqual(result['cwd'], CWD)

    def test_old_command_is_not_active_ini_evidence(self):
        for command in ['"' + EXE + '"', '"' + EXE + '" "C:\\old.ini"', 'unfamiliar old launch']:
            p, t, gate = self.run_bound(old_command=command)
            self.assertTrue(gate(json.dumps(p))['ok'])
            self.assertEqual(t.files[BASE], POST_EXIT)

    def test_exit_failure_prevents_reads_writes_and_restart(self):
        p, t, gate = self.run_bound(stay=True)
        result = gate(json.dumps(p))
        self.assertFalse(result['ok'])
        self.assertNotIn('snapshot', t.calls)
        self.assertEqual(t.files, {BASE: RAW})

    def test_every_failed_exchange_prevents_next_operation(self):
        for index in range(1, 15):
            p, t, gate = self.run_bound(fail=index)
            result = gate(json.dumps(p))
            self.assertFalse(result['ok'], index)
            self.assertEqual(len(t.calls), index + 1)
            self.assertEqual(t.calls[-1], 'dispose')
            self.assertNotIn('restore', t.calls)
            self.assertLessEqual(t.calls.count('start'), 1)

    def test_pid_creation_exe_multiple_and_malformed_fail_before_close(self):
        for change in [dict(pid=43), dict(created='2026-09-09T00:00:00.0000000Z'), dict(exe=r'C:\other.exe')]:
            p, t, gate = self.run_bound()
            t.rows[0].update(change)
            self.assertFalse(gate(json.dumps(p))['ok'])
            self.assertNotIn('close', t.calls)
        for rows in [None, [], [{}, {}], [{}]]:
            p, t, gate = self.run_bound()
            t.rows = rows
            self.assertFalse(gate(json.dumps(p))['ok'])
            self.assertNotIn('close', t.calls)

    def test_dry_cli_no_factory_or_llm_and_execute_needs_exclusivity(self):
        args = ['--exe', EXE, '--baseline', BASE, '--cwd', CWD, '--pid', '42', '--created', CREATED]
        def forbidden(*a):
            self.fail('dry-run made external call')
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(trial.main(args, executor_factory=forbidden, llm=forbidden), 0)
        self.assertEqual(json.loads(out.getvalue())['mode'], 'dry_run')
        with redirect_stdout(io.StringIO()):
            self.assertNotEqual(trial.main(args + ['--execute'], executor_factory=forbidden, llm=forbidden), 0)

    def test_transport_cleanup_failure_cannot_report_success(self):
        p, t, gate = self.run_bound()
        def fail_close():
            raise OSError('sensitive transport diagnostic')
        t.close = fail_close
        result = gate(json.dumps(p))
        self.assertFalse(result['ok'])
        self.assertNotIn('sensitive', json.dumps(result))

    def test_new_process_mismatch_and_midflight_identity_change(self):
        for op, mutation in [('start', 'command'), ('start', 'created'), ('query', 'created')]:
            p, t, gate = self.run_bound()
            exchange = t.exchange
            def corrupt(request):
                response = exchange(request)
                if request['op'] == op:
                    if op == 'start':
                        response['value'][mutation] = CREATED if mutation == 'created' else 'wrong command'
                    elif t.calls.count('query') == 2:
                        response['value'][0]['created'] = '2026-09-09T00:00:00.0000000Z'
                return response
            t.exchange = corrupt
            self.assertFalse(gate(json.dumps(p))['ok'])
            self.assertLessEqual(t.calls.count('start'), 1)
            if op == 'query':
                self.assertNotIn('close', t.calls)

    def test_cli_execute_llm_gate_and_no_model_mutation(self):
        args = ['--exe', EXE, '--baseline', BASE, '--cwd', CWD, '--pid', '42', '--created', CREATED,
                '--execute', '--exclusive-operator']
        calls = []
        def factory(bound):
            calls.append(bound)
            return trial.WindowsExecutor(bound, Transport(bound))
        def malicious(bound, url, model):
            bound['pid'] = 777
            return json.dumps(bound)
        with redirect_stdout(io.StringIO()):
            self.assertEqual(trial.main(args, executor_factory=factory, llm=malicious), 2)
        self.assertEqual(calls, [])
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(trial.main(args, executor_factory=factory,
                                       llm=lambda p, u, m: json.dumps(p)), 0)
        self.assertTrue(json.loads(out.getvalue())['ok'])
        self.assertEqual(len(calls), 1)

    def test_generated_ps_is_narrow_normal_close_and_explicit_launch(self):
        ps = trial.PS_SERVER
        for required in ['CloseMainWindow()', 'WaitForExit(10000)', 'AssertAbsent',
                         '$handle = $p.Handle', 'CreateNew', '$plan.trial', '$plan.cwd',
                         'AssertProcess', 'FileIdentity', 'CheckPath']:
            self.assertIn(required, ps)
        for forbidden in ['.Kill(', 'Stop-Process', 'File]::Replace', 'Copy-Item', 'Invoke-Expression']:
            self.assertNotIn(forbidden, ps)

WINDOWS_PARSER = '--windows-parser' in sys.argv
if WINDOWS_PARSER:
    sys.argv.remove('--windows-parser')

@unittest.skipUnless(WINDOWS_PARSER, 'opt-in Windows parser only; no lifecycle execution')
class PowerShellParser(unittest.TestCase):
    def test_generated_script_parser_only(self):
        import base64
        import subprocess
        import tempfile
        # Actual generated source is stdin DATA. ParseInput returns AST only;
        # no ScriptBlock.Create/Invoke, dot-source, or lifecycle execution.
        parser = r"""
$source = [Console]::In.ReadToEnd()
$tokens = $null; $errors = $null
$null = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
[Console]::WriteLine(('syntax errors: ' + $errors.Count))
if ($errors.Count) { exit 1 }
"""
        encoded = base64.b64encode(parser.encode('utf-16le')).decode('ascii')
        with tempfile.TemporaryDirectory(prefix='np21w-trial-parser-') as tmp:
            source = Path(tmp) / 'source.txt'
            source.write_bytes(trial.PS_SERVER.encode('utf-8'))
            with source.open('rb') as data:
                result = subprocess.run(['powershell.exe', '-NoLogo', '-NoProfile', '-NonInteractive',
                                         '-EncodedCommand', encoded], stdin=data,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
        self.assertIn(b'syntax errors: 0', result.stdout)

if __name__ == '__main__':
    unittest.main()
