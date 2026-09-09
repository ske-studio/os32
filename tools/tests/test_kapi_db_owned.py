"""Host-test the real DB wrapper; SQLite fault injection, no OS/VFS access."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class DbOwnedTests(unittest.TestCase):
    def test_host_cases(self):
        with tempfile.TemporaryDirectory(prefix="os32-db-f1-") as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "memmap.h").write_text(
                "extern unsigned char test_shm[];\n#define MEM_SHM_BASE test_shm\n")
            (tmp / "kstring.h").write_text(
                "#include <string.h>\n#define kstrncpy(d,s,n) strncpy(d,s,n)\n")
            (tmp / "kprintf.h").write_text("int kprintf(int color, const char *fmt, ...);\n")
            exe = tmp / "db-owned"
            subprocess.run([
                "cc", "-std=gnu89", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wdeclaration-after-statement", "-D__cdecl=",
                "-I" + str(tmp), "-I" + str(ROOT / "sdk/include/os32"),
                "-I" + str(ROOT / "include"), "-I" + str(ROOT / "fs"),
                "-I" + str(ROOT / "lib/sqlite3"),
                str(ROOT / "tools/tests/kapi_db_owned_host.c"), "-o", str(exe)
            ], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
