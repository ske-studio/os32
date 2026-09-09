"""Actual paging.c, ILP32; only privileged asm replaced for host execution."""
import pathlib
import subprocess
import tempfile
import argparse
parser = argparse.ArgumentParser()
parser.add_argument('--rebuild', choices=['nonmaster', 'rollback', 'sparse', 'attrs', 'final'])
args = parser.parse_args()
ROOT = pathlib.Path(__file__).resolve().parents[2]
FLAGS = ['-m32', '-march=i386', '-std=gnu89', '-ffreestanding', '-fno-pie', '-fno-stack-protector', '-Wall', '-Wextra', '-Werror', '-Wdeclaration-after-statement']
with tempfile.TemporaryDirectory(prefix='os32-paging-') as tmp:
    tmp = pathlib.Path(tmp)
    source = (ROOT / 'kernel/paging.c').read_text()
    source = source.replace('__asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));', 'cr3_val = host_cr3;')
    source = source.replace('__asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");', 'host_cr3 = cr3_val;')
    source = source.replace('__asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");', 'host_cr3 = pd_phys;')
    source = source.replace('__asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));', 'cr0_val = 0;')
    source = source.replace('__asm__ volatile("mov %0, %%cr0" : : "r"(cr0_val) : "memory");', '(void)cr0_val;')
    (tmp / 'paging_host_source.c').write_text(source)
    allocator = (ROOT / 'kernel/pgalloc.c').read_text()
    allocator = allocator.replace('irq_save()', '0').replace('irq_restore(flags)', '(void)flags')
    (tmp / 'pgalloc_host_source.c').write_text(allocator)
    includes = ['-I' + str(ROOT / p) for p in ('include', 'kernel', 'lib')] + ['-I' + str(tmp)]
    exe = tmp / 'paging'
    harness = 'paging_rebuild_host.c' if args.rebuild else 'paging_bounds_host.c'
    defines = ['-DPHYSMEM_HOST_TEST=1']
    if args.rebuild:
        defines.append('-DTEST_' + args.rebuild.upper())
    subprocess.run(['gcc', *FLAGS, *defines, '-nostdlib', '-static', '-no-pie', *includes, str(ROOT / 'tools/tests' / harness), str(ROOT / 'kernel/physmem.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
    subprocess.run(['i386-elf-gcc', *FLAGS, '-O2', *includes, '-c', str(ROOT / 'kernel/paging.c'), '-o', str(tmp / 'paging.o')], check=True)
    print('HOST ILP32 + TARGET GNU89 PASS')
