#!/bin/bash
tail -n 100 boot_hdd_root.asm > test_tail.asm
head -n 100 boot_hdd_root.asm > test_head.asm
# Wait, this is getting complex. I'll just remove half the functions.
