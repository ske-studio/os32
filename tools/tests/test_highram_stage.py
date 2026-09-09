"""Real model + allocator + paging + sys; only privileged I/O substituted.
32/64 here denote modeled MiB; execution uses real target ILP32 types.
"""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]

class Stage(unittest.TestCase):
    def run_case(self, case='success', end=16384):
        with tempfile.TemporaryDirectory(prefix='os32-stage-') as d:
            d = pathlib.Path(d)
            for unit in ('paging', 'pgalloc', 'sys'):
                s = (ROOT / f'kernel/{unit}.c').read_text()
                s = s.replace('irq_save()', 'host_irq_save()').replace('irq_restore(flags)', 'host_irq_restore(flags)')
                s = s.replace('__asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));', 'cr3_val = host_cr3;')
                for name in ('cr3_val', 'pd_phys'):
                    s = s.replace(f'__asm__ volatile("mov %0, %%cr3" : : "r"({name}) : "memory");', f'host_cr3 = {name};')
                s = s.replace('__asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));', 'cr0_val = 0;')
                s = s.replace('__asm__ volatile("mov %0, %%cr0" : : "r"(cr0_val) : "memory");', '(void)cr0_val;')
                (d / f'{unit}_host_source.c').write_text(s)
            cmd = ['gcc', '-m32', '-march=i386', '-std=gnu89', '-Wall', '-Wextra', '-Werror', '-Wdeclaration-after-statement', '-ffreestanding', '-fno-pie', '-fno-stack-protector', '-nostdlib', '-static', '-no-pie', '-ffunction-sections', '-Wl,--gc-sections', '-DPHYSMEM_HOST_TEST=1', f'-DTEST_{case.upper()}', f'-DTEST_END={end}']
            cmd += ['-I' + str(ROOT / p) for p in ('include', 'kernel', 'lib', 'drivers', 'sdk/include/os32')] + ['-I' + str(d)]
            subprocess.run(cmd + [str(ROOT / 'tools/tests/highram_stage_host.c'), str(ROOT / 'kernel/physmem.c'), '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True, timeout=20)

    def test_irq_off(self):
        self.run_case('irq_off')

    def test_layout_snapshot(self):
        self.run_case('layout_alias')

    def test_model_workspace_alias(self):
        self.run_case('model_ws_alias')

    def test_atomic_layout(self):
        self.run_case('atomic')

    def test_stage_context(self):
        for case in ('nonmaster', 'live'):
            for end in (8192, 16384):
                self.run_case(case, end)

    def test_workspace_failure(self):
        for case in ('oom', 'late_oom', 'unmapped_ws'):
            self.run_case(case)

    def test_reject_bad_high_pde(self):
        self.run_case('bad_high_pde')

    def test_reserve_top_on_model_path(self):
        for end in (8192, 16384):
            self.run_case('reserve_top', end)

    def test_success_32_64(self):
        for end in (8192, 16384):
            self.run_case(end=end)

if __name__ == '__main__':
    unittest.main()
