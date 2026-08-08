"""HTTP client for the NP21/W embedded AI debug server.

WSL cannot reach the Windows localhost port directly (firewall), so requests
go through the Windows curl.exe. This layer is intentionally thin so it can be
swapped for httpx once the firewall is opened.
"""

import subprocess

CURL = "/mnt/c/Windows/System32/curl.exe"
BASE = "http://127.0.0.1:8025"


class EmuError(Exception):
    pass


def _run(args, timeout):
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout)
    except FileNotFoundError:
        raise EmuError("curl.exe not found at %s" % CURL)
    except subprocess.TimeoutExpired:
        raise EmuError("request timed out")
    if p.returncode != 0:
        raise EmuError("curl failed (rc=%d): %s" %
                       (p.returncode,
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
