"""HTTP client for the NP21/W embedded AI debug server.

Prefers a direct connection from WSL. That works when WSL runs in mirrored
networking mode (``networkingMode=mirrored`` in ``.wslconfig``), where the
Windows loopback is reachable as 127.0.0.1. Under NAT mode -- or when a
firewall rule blocks the port -- it falls back to the Windows curl.exe, which
always reaches the emulator because it runs on the Windows side.

The probe result is cached for the life of the process.
"""

import os
import subprocess
import urllib.error
import urllib.request

CURL = "/mnt/c/Windows/System32/curl.exe"
# must match aidbport= in np21x64w.ini
BASE = os.environ.get("NP21W_AIDEBUG_URL", "http://127.0.0.1:8025")

_direct = None  # None = not probed yet, True/False = probe result


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


def _direct_available():
    """Probe once whether WSL can reach the emulator without Windows curl."""
    global _direct
    if _direct is None:
        try:
            urllib.request.urlopen(BASE + "/api/status", timeout=3).read()
            _direct = True
        except Exception:
            _direct = False
    return _direct


def _http(path, body=None, timeout=20):
    """Direct request. body=None -> GET, otherwise POST. Returns bytes."""
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else body.encode("utf-8")
    req = urllib.request.Request(BASE + path, data=data,
                                 method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        # The API reports its own errors as JSON bodies; hand them back
        # rather than raising, so callers see the message.
        return e.read()
    except Exception as exc:
        raise EmuError("direct request to %s failed: %s" % (BASE + path, exc))


def get(path, timeout=15):
    """GET path (may include ?query); returns response bytes."""
    if _direct_available():
        return _http(path, None, timeout)
    return _run([CURL, "-s", "-m", str(timeout), BASE + path], timeout + 5)


def post(path, body=None, timeout=20):
    """POST path with an optional raw string body; returns response bytes."""
    if _direct_available():
        return _http(path, body if body is not None else b"", timeout)
    args = [CURL, "-s", "-m", str(timeout), "-X", "POST", BASE + path]
    if body is not None:
        args += ["--data-binary", body]
    return _run(args, timeout + 5)


def get_to_file(path, out_path, timeout=20):
    """GET path, writing the (possibly binary) body to out_path."""
    if _direct_available():
        with open(out_path, "wb") as f:
            f.write(_http(path, None, timeout))
        return out_path
    _run([CURL, "-s", "-m", str(timeout), "-o", out_path, BASE + path],
         timeout + 5)
    return out_path
