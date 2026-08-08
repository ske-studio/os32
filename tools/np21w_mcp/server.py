#!/usr/bin/env python3
"""NP21/W AI debug MCP server (stdio, dependency-free).

Exposes the emulator's embedded HTTP debug API as MCP tools, adding
kernel.map symbol resolution: addresses may be given as symbol names
(e.g. "exec_run", "timer_handler+0x10") and returned EIPs are annotated
as symbol+offset.

Speaks the MCP stdio transport (newline-delimited JSON-RPC 2.0) using only
the Python standard library, so no `pip install` is required.
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import np21w_client as emu           # noqa: E402
from symbols import SymbolTable      # noqa: E402

MAP_PATH = os.environ.get(
    "OS32_KERNEL_MAP",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "..", "build", "out", "kernel.map"))
SYMS = SymbolTable(os.path.normpath(MAP_PATH))

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "np21w-aidebug", "version": "1.0.0"}


# --------------------------------------------------------------------------
# tool helpers
# --------------------------------------------------------------------------

def _json(body):
    return json.loads(body.decode("utf-8", "replace"))


def _annotate_eip(obj, keys=("eip", "prev_eip")):
    for k in keys:
        v = obj.get(k)
        if isinstance(v, str) and v.startswith("0x"):
            sym = SYMS.annotate(int(v, 16))
            if sym:
                obj[k + "_sym"] = sym
    return obj


def _resolve_or_error(token):
    addr = SYMS.resolve(token)
    if addr is None:
        raise emu.EmuError("unknown symbol: %s" % token)
    return addr


# --------------------------------------------------------------------------
# tools
# --------------------------------------------------------------------------

def tool_status(_):
    return _annotate_eip(_json(emu.get("/api/status")))


def tool_regs(_):
    return _annotate_eip(_json(emu.get("/api/regs")))


def tool_tvram(_):
    d = _json(emu.get("/api/tvram"))
    return "\n".join(d.get("lines", []))


def tool_read_mem(args):
    addr = _resolve_or_error(args["addr"])
    length = int(args.get("len", 64))
    space = args.get("space", "phys")
    q = "/api/mem?addr=0x%x&len=%d&space=%s" % (addr, length, space)
    if space == "virt" and "seg" in args:
        q += "&seg=" + args["seg"]
    return _json(emu.get(q))


def tool_write_mem(args):
    addr = _resolve_or_error(args["addr"])
    space = args.get("space", "phys")
    q = "/api/mem?addr=0x%x&space=%s" % (addr, space)
    return _json(emu.post(q, args["hex"]))


def tool_disasm(args):
    q = "/api/disasm?n=%d" % int(args.get("n", 8))
    if "addr" in args:
        q += "&addr=0x%x" % _resolve_or_error(args["addr"])
    return _json(emu.get(q))


def tool_cmd(args):
    return emu.post("/api/cmd", args["command"], timeout=30).decode(
        "utf-8", "replace")


def tool_key(args):
    body = []
    if "seq" in args:
        body.append("seq=" + args["seq"])
    if "text" in args:
        body.append("text=" + args["text"])
    return _json(emu.post("/api/key", "&".join(body)))


def tool_pause(_):
    return _json(emu.post("/api/pause"))


def tool_resume(_):
    return _json(emu.post("/api/resume"))


def tool_reset(_):
    return _json(emu.post("/api/reset"))


def tool_break_add(args):
    addr = _resolve_or_error(args["addr"])
    q = "/api/break/add?addr=0x%x" % addr
    if "cs" in args:
        q += "&cs=" + args["cs"]
    return _json(emu.post(q))


def tool_break_del(args):
    if args.get("all"):
        return _json(emu.post("/api/break/del?addr=*"))
    addr = _resolve_or_error(args["addr"])
    return _json(emu.post("/api/break/del?addr=0x%x" % addr))


def tool_break_list(_):
    d = _json(emu.get("/api/break"))
    for bp in d.get("breakpoints", []):
        sym = SYMS.annotate(int(bp["eip"], 16))
        if sym:
            bp["eip_sym"] = sym
    return d


def tool_step(args):
    d = _json(emu.post("/api/step?n=%d" % int(args.get("n", 1))))
    return _annotate_eip(d)


def tool_trace(_):
    d = _json(emu.get("/api/trace"))
    for e in d.get("trace", []):
        sym = SYMS.annotate(int(e["eip"], 16))
        if sym:
            e["eip_sym"] = sym
    return d


def tool_trace_start(_):
    return _json(emu.post("/api/trace/start"))


def tool_trace_stop(_):
    return _json(emu.post("/api/trace/stop"))


def tool_state_save(args):
    return _json(emu.post("/api/state/save", "file=" + args["file"]))


def tool_state_load(args):
    return _json(emu.post("/api/state/load", "file=" + args["file"]))


def tool_screenshot(_):
    path = os.path.join(tempfile.gettempdir(), "np21w_shot.bmp")
    emu.get_to_file("/api/screenshot", path)
    return {"saved": path, "note": "BMP written to host filesystem"}


# --------------------------------------------------------------------------
# tool registry (name -> (handler, description, input schema))
# --------------------------------------------------------------------------

def _obj(props, required=None):
    return {"type": "object", "properties": props, "required": required or []}


_ADDR = {"type": "string",
         "description": "hex address or kernel.map symbol (e.g. exec_run, "
                        "timer_handler+0x10, 0x100000)"}

TOOLS = {
    "emu_status": (tool_status,
                   "Execution state, CS:EIP (symbol-annotated), CPU mode.",
                   _obj({})),
    "emu_regs": (tool_regs,
                 "All CPU registers, segment bases, CR0-4, GDTR/IDTR; EIP "
                 "annotated with symbol+offset.",
                 _obj({})),
    "emu_tvram": (tool_tvram,
                  "Text VRAM as UTF-8 (the on-screen text; screenshot "
                  "replacement).",
                  _obj({})),
    "emu_read_mem": (tool_read_mem,
                     "Read guest memory as hex.",
                     _obj({"addr": _ADDR,
                           "len": {"type": "integer", "default": 64},
                           "space": {"type": "string",
                                     "enum": ["phys", "linear", "virt"],
                                     "default": "phys"},
                           "seg": {"type": "string",
                                   "description": "segment for space=virt"}},
                          ["addr"])),
    "emu_write_mem": (tool_write_mem,
                      "Write guest memory (hex string).",
                      _obj({"addr": _ADDR,
                            "hex": {"type": "string"},
                            "space": {"type": "string", "default": "phys"}},
                           ["addr", "hex"])),
    "emu_disasm": (tool_disasm,
                   "Disassemble n instructions (note: internal disassembler "
                   "has known boundary quirks; prefer objdump on kernel.elf).",
                   _obj({"addr": _ADDR,
                         "n": {"type": "integer", "default": 8}})),
    "emu_cmd": (tool_cmd,
                "Run a shell command in the OS32 rshell over the embedded "
                "serial bridge; returns its output.",
                _obj({"command": {"type": "string"}}, ["command"])),
    "emu_key": (tool_key,
                "Inject PC-98 keys. seq is comma-separated chords "
                "(SHIFT+SPACE,A,ENTER); text is literal ASCII.",
                _obj({"seq": {"type": "string"},
                      "text": {"type": "string"}})),
    "emu_pause": (tool_pause, "Pause emulation (frame granularity).", _obj({})),
    "emu_resume": (tool_resume,
                   "Resume from a pause or a breakpoint.", _obj({})),
    "emu_reset": (tool_reset, "Hard-reset the emulated machine.", _obj({})),
    "emu_break_add": (tool_break_add,
                      "Set a breakpoint at an address/symbol.",
                      _obj({"addr": _ADDR, "cs": {"type": "string"}},
                           ["addr"])),
    "emu_break_del": (tool_break_del,
                      "Delete a breakpoint (or all=true).",
                      _obj({"addr": _ADDR, "all": {"type": "boolean"}})),
    "emu_break_list": (tool_break_list,
                       "List breakpoints (EIP symbol-annotated).", _obj({})),
    "emu_step": (tool_step,
                 "Single-step n instructions (must be paused at a "
                 "breakpoint); returns the new EIP.",
                 _obj({"n": {"type": "integer", "default": 1}})),
    "emu_trace": (tool_trace,
                  "Get the CS:EIP execution trace (symbol-annotated).",
                  _obj({})),
    "emu_trace_start": (tool_trace_start,
                        "Start recording the CS:EIP trace.", _obj({})),
    "emu_trace_stop": (tool_trace_stop,
                       "Stop recording the CS:EIP trace.", _obj({})),
    "emu_state_save": (tool_state_save,
                       "Save emulator state to a file.",
                       _obj({"file": {"type": "string"}}, ["file"])),
    "emu_state_load": (tool_state_load,
                       "Load emulator state from a file.",
                       _obj({"file": {"type": "string"}}, ["file"])),
    "emu_screenshot": (tool_screenshot,
                       "Capture the screen to a BMP on the host filesystem.",
                       _obj({})),
}


# --------------------------------------------------------------------------
# JSON-RPC / MCP plumbing
# --------------------------------------------------------------------------

def _send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def _result(req_id, result):
    _send({"jsonrpc": "2.0", "id": req_id, "result": result})


def _error(req_id, code, message):
    _send({"jsonrpc": "2.0", "id": req_id,
           "error": {"code": code, "message": message}})


def _handle(msg):
    method = msg.get("method")
    req_id = msg.get("id")

    if method == "initialize":
        _result(req_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": SERVER_INFO,
        })
    elif method == "notifications/initialized":
        pass
    elif method == "tools/list":
        tools = [{"name": n, "description": d, "inputSchema": s}
                 for n, (_, d, s) in TOOLS.items()]
        _result(req_id, {"tools": tools})
    elif method == "tools/call":
        params = msg.get("params", {})
        name = params.get("name")
        args = params.get("arguments", {}) or {}
        entry = TOOLS.get(name)
        if entry is None:
            _error(req_id, -32601, "unknown tool: %s" % name)
            return
        handler = entry[0]
        try:
            out = handler(args)
        except emu.EmuError as e:
            _result(req_id, {"content": [{"type": "text",
                                          "text": "error: %s" % e}],
                             "isError": True})
            return
        except Exception as e:  # noqa: BLE001
            _result(req_id, {"content": [{"type": "text",
                                          "text": "internal error: %s" % e}],
                             "isError": True})
            return
        text = out if isinstance(out, str) else json.dumps(
            out, indent=2, ensure_ascii=False)
        _result(req_id, {"content": [{"type": "text", "text": text}]})
    elif req_id is not None:
        _error(req_id, -32601, "method not found: %s" % method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        _handle(msg)


if __name__ == "__main__":
    main()
