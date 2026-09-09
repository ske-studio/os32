#!/usr/bin/env python3
"""Compile unchanged gshell modules against an explicitly mocked host ABI.
No SDK edits; generated adapters live in target/t5b-host only.
"""
from pathlib import Path
import re
import subprocess
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'userland/gshell/target/t5b-host'
OUT.mkdir(parents=True, exist_ok=True)
def run(args):
    subprocess.run(args, cwd=ROOT, check=True)
sdk = ROOT / 'sdk/rust/os32api/src'
source = (sdk / 'lib.rs').read_text()
abi = (sdk / 'kapi_generated.rs').read_text()
fields = re.findall(r'pub (\w+): (.+),', abi.split('pub struct KernelAPI {')[1].split('\n}')[0])
init = []
for name, ty in fields:
    value = ('core::mem::transmute::<*const (), ' + ty + '>(unexpected as *const ())') if ty.startswith('unsafe ') else 'core::mem::zeroed()'
    init.append(name + ': ' + value)
adapter = f'''#![allow(dead_code)]
#[path="{sdk / 'kapi_generated.rs'}"] pub mod kapi_generated;
pub use kapi_generated::*;
#[path="{sdk / 'gui/mod.rs'}"] pub mod gui;
use core::cell::UnsafeCell;
''' + source[source.index('struct ApiHolder'):source.index('/* ================================================================ */', source.index('pub unsafe fn api_ptr'))] + source[source.index('pub mod gfx {'):]
adapter += '\nunsafe extern "C" fn unexpected() { panic!("unmocked KAPI reached"); }\npub fn mock_api() -> KernelAPI { unsafe { KernelAPI {' + ',\n'.join(init) + '} } }\n'
(OUT / 'host_api.rs').write_text(adapter)
run(['rustc', '--edition=2021', '--crate-type=rlib', '--crate-name=os32api', str(OUT / 'host_api.rs'), '-o', str(OUT / 'libos32api.rlib')])
for name in ['libos32term', 'libos32term_render']:
    args = ['rustc','--edition=2021','--crate-type=rlib','--crate-name',name,str(ROOT/'userland'/name/'src/lib.rs'),'-o',str(OUT/(name+'.rlib'))]
    if name.endswith('_render'): args += ['--extern','libos32term='+str(OUT/'libos32term.rlib')]
    run(args)
root = (ROOT / 'userland/gshell/src/lib.rs').read_text().replace('#![no_std]', '#![allow(dead_code)]').replace('#[no_mangle]', '').replace('pub extern "C" fn main(', 'pub extern "C" fn guest_main(')
root = re.sub(r'^mod (\w+);', lambda m: '#[path="' + str(ROOT/'userland/gshell/src'/(m[1]+'.rs')) + '"] mod '+m[1]+';', root, flags=re.M)
root += '\n#[path="'+str(ROOT/'userland/gshell/host/mocks.rs')+'"] mod mocks;\n'
(OUT / 'integration_root.rs').write_text(root)
run(['rustc','--edition=2021','--test',str(OUT/'integration_root.rs'),'-L',str(OUT), '--extern','os32api='+str(OUT/'libos32api.rlib'),'--extern','libos32term='+str(OUT/'libos32term.rlib'),'--extern','libos32term_render='+str(OUT/'libos32term_render.rlib'),'-o',str(OUT/'integration-tests')])
run([str(OUT/'integration-tests'),'--test-threads=1'])
