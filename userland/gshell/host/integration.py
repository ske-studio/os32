#!/usr/bin/env python3
"""Compile unchanged gshell modules against an explicitly mocked host ABI.
No SDK edits; generated adapters live in target/gshell-host only.
"""
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'userland/gshell/target/gshell-host'
OUT.mkdir(parents=True, exist_ok=True)
def run(args):
    subprocess.run(args, cwd=ROOT, check=True)
sys.path.insert(0, str(ROOT / 'tools/tests'))
import os32api_host
os32api_host.build(OUT)
root = (ROOT / 'userland/gshell/src/lib.rs').read_text().replace('#![no_std]', '#![allow(dead_code)]').replace('#[no_mangle]', '').replace('pub extern "C" fn main(', 'pub extern "C" fn guest_main(')
root = re.sub(r'^mod (\w+);', lambda m: '#[path="' + str(ROOT/'userland/gshell/src'/(m[1]+'.rs')) + '"] mod '+m[1]+';', root, flags=re.M)
root += '\n#[path="'+str(ROOT/'userland/gshell/host/mocks.rs')+'"] mod mocks;\n'
(OUT / 'integration_root.rs').write_text(root)
run(['rustc','--edition=2021','--test',str(OUT/'integration_root.rs'),'-L',str(OUT), '--extern','os32api='+str(OUT/'libos32api.rlib'),'-o',str(OUT/'integration-tests')])
run([str(OUT/'integration-tests'),'--test-threads=1'])
