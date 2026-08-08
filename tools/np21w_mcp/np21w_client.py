"""HTTP client for the NP21/W embedded AI debug server.

WSL cannot reach the Windows localhost port directly (firewall), so requests
go through the Windows curl.exe. This layer is intentionally thin so it can be
swapped for httpx once the firewall is opened.
"""

import os
import subprocess

CURL = "/mnt/c/Windows/System32/curl.exe"
# must match aidbport= in np21x64w.ini
BASE = os.environ.get("NP21W_AIDEBUG_URL", "http://127.0.0.1:8025")


class EmuError(Exception):
    pass


def _hint(rc):
    """Translate a curl exit code into something actionable, or None."""
    if rc == 7:
        return ("cannot connect to %s -- NP21/W is probably not running, or "
                "its aidebug server is off (check aidebug=true / aidbport in "
                "np21x64w.ini)" % BASE)
    if rc == 28:
        return ("request timed out -- the emulator may be stopped at a "
                "breakpoint or busy; try emu_status")
    if rc == 56:
        return "connection reset by the emulator (it may have just exited)"
    return None


def _run(args, timeout):
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout)
    except FileNotFoundError:
        raise EmuError("curl.exe not found at %s" % CURL)
    except subprocess.TimeoutExpired:
        raise EmuError("request timed out")
    if p.returncode != 0:
        raise EmuError(_hint(p.returncode) or
                       "curl failed (rc=%d): %s"
                       % (p.returncode,
                          p.stderr.decode("utf-8", "replace")[:200]))
    return p.stdout


def get(path, timeout=15):
    """GET path (may include ?query); returns response bytes."""
    return _run([CURL, "-s", "-m", str(timeout), BASE + path], timeout + 5)


def post(path, body=None, timeout=20):
    """POST path with an optional raw string body; returns response bytes."""
    args = [CURL, "-s", "-m", str(timeout), "-X", "POST", BASE + path]
    if body is not None:
        args += ["--data-binary", body]
    return _run(args, timeout + 5)


def get_to_file(path, out_path, timeout=20):
    """GET path, writing the (possibly binary) body to out_path."""
    _run([CURL, "-s", "-m", str(timeout), "-o", out_path, BASE + path],
         timeout + 5)
    return out_path
