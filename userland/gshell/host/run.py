#!/usr/bin/env python3
"""Offline host tests for the production display-only boundary."""
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'userland/gshell/target/t5b-host'
OUT.mkdir(parents=True, exist_ok=True)
def run(args):
    subprocess.run(args, cwd=ROOT, check=True)
for name in ['libos32term', 'libos32term_render']:
    args = ['rustc', '--edition=2021', '--crate-type=rlib', '--crate-name', name,
            str(ROOT / 'userland' / name / 'src/lib.rs'), '-o', str(OUT / (name + '.rlib'))]
    if name.endswith('_render'):
        args += ['--extern', 'libos32term=' + str(OUT / 'libos32term.rlib')]
    run(args)
run(['rustc', '--edition=2021', '--test', '--cfg', 't5b_core_test', 'userland/gshell/src/terminal.rs',
     '--extern', 'libos32term=' + str(OUT / 'libos32term.rlib'),
     '--extern', 'libos32term_render=' + str(OUT / 'libos32term_render.rlib'),
     '-L', str(OUT), '-o', str(OUT / 'terminal-tests')])
run([str(OUT / 'terminal-tests'), '--test-threads=1'] + sys.argv[1:])
