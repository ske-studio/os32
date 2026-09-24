"""SerialFS (シリアル越しの /host) のホスト試験 — 票 TASK_SERIAL_HOSTFS §3 の T1 と T5。

記録: tools/tests/serialfs_tdd.md
票  : docs/tasks/realhw/TASK_SERIAL_HOSTFS.md 部品 B (§1-v2 / §1-v3 / ユーザー決裁 2026-09-24)

4 つの段:

  C   ゲスト側の実物 (fs/sfs_proto.c・fs/sfs_client.c・fs/serialfs.c・
      userland/shell/serial_watchdog.c・userland/system/hsync_bootold.inc) を
      tools/tests/serialfs_host.c に取り込み、偽の線・仮想の時計・偽のホストで
      回す。フレーム・CRC32・番号・再送・フレーム長の境界・VFS の契約・
      障害の注入 (喪失 / 遅延 / CRC 破損 / 番号違い / セッション違い /
      ホストの停止 / ごみの連続 / ERR / 契約違反の応答)
  GATE drivers/serial.c のゲート (tools/tests/serial_gate_host.c)。セッション中に
      rshell へフレームが漏れない・出力は保留リングへ・下ろすと受信を捨てる
  PY  ホスト側の実物 (tools/serialfs_host.py・tools/rshell_serial.py): 振り分け
      (EOT とフレームの混在)、`..` と symlink の脱出の拒否、応答のキャッシュ
      (再送で副作用が二重にならない)、期限切れの応答を送らない、BYE の後は
      答えない、`sfs run` の行の外ではフレームを解釈しない
  XC  C と Python の相互照合 (C が組んだ列を Python が読む / 逆)、定数の照合
      (sfs_proto.h ⇔ serialfs_host.py、hsync_bootold.inc ⇔ boot/boot_defs.h)、
      結合 (C の受け手と Python のホストを実時間のパイプでつなぎ、応答の喪失と
      CRC 破損を注入して RENAME が二重に実行されないこと)

  python3 -B tools/tests/test_serialfs.py                     # 全段
  python3 -B tools/tests/test_serialfs.py --target            # + i386-elf -Werror
  python3 -B tools/tests/test_serialfs.py --mutate            # + 否定側

--mutate: 実装を 1 か所ずつ**写しの上で**壊して RED になることを見る。組めない
変異・当てはまらない変異は ERROR として数える (見逃しと同じく失敗)。何も変えない
恒等の対照を C と Python に 1 本ずつ入れ、それが GREEN であること (試験の枠が
壊れていないこと) も見る。
"""
import importlib.util
import io
import os
import pathlib
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
HARNESS = ROOT / "tools/tests/serialfs_host.c"
GATE_HARNESS = ROOT / "tools/tests/serial_gate_host.c"

C_SRCS = {
    "fs/sfs_proto.c": "sfs_proto.c",
    "fs/sfs_proto.h": "sfs_proto.h",
    "fs/sfs_client.c": "sfs_client.c",
    "fs/sfs_client.h": "sfs_client.h",
    "fs/serialfs.c": "serialfs.c",
    "userland/shell/serial_watchdog.c": "serial_watchdog.c",
    "userland/system/hsync_bootold.inc": "hsync_bootold.inc",
}
C_CASES = ["crc_and_frames", "timing_and_seq", "vfs_contract", "mount_permit",
           "fault_drop", "fault_corrupt", "fault_stray", "fault_delay",
           "fault_dead", "fault_err_stale", "fault_garbage", "fault_stray_flood",
           "cannot_wait",
           "seq_exhausted", "hello_cases", "bad_host_replies", "rshell_rules",
           "bootold_rules"]
GATE_CASES = ["gate_tx", "hold_overflow", "gate_rx", "gate_init_refused",
              "isr_counts"]

FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement", "-D_DEFAULT_SOURCE", "-D__cdecl=",
         "-Wno-unused-function"]

def flags_for(mutated):
    """実物は -Werror。変異は警告 (使われなくなった引数など) を止めない —
    **組めない変異は ERROR** として数えるので、警告で ERROR にしない。"""
    if mutated is None:
        return FLAGS
    return [f for f in FLAGS if f != "-Werror"]


sys.path.insert(0, str(ROOT / "tools/tests"))
from test_serial_portc import FAKE_ARCH_IO, FAKE_PLATFORM_IO  # noqa: E402


# ============================================================================
#  C の組み立て
# ============================================================================
def c_build(tmp, mutated=None, name="real"):
    """serialfs_host.c を組む。mutated = {rel: 中身} は写しの上で差し替える
    (写しのディレクトリを -I の先頭に置く。実物は 1 バイトも触らない)。"""
    work = pathlib.Path(tmp) / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    for rel, base in C_SRCS.items():
        text = (mutated or {}).get(rel, (ROOT / rel).read_text(encoding="utf-8"))
        (work / base).write_text(text, encoding="utf-8")
    exe = work / "serialfs-host"
    cmd = ["gcc", *flags_for(mutated), "-I" + str(work), "-I" + str(ROOT / "fs"),
           "-I" + str(ROOT / "lib"), "-I" + str(ROOT / "include"),
           "-I" + str(ROOT / "sdk/include/os32"),
           "-I" + str(ROOT / "userland/shell"),
           "-I" + str(ROOT / "userland/system"),
           str(HARNESS), "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True,
                   stderr=subprocess.DEVNULL if mutated is not None else None)
    return exe


def gate_build(tmp, mutated=None, name="gate"):
    work = pathlib.Path(tmp) / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    (work / "arch_io.h").write_text(FAKE_ARCH_IO, encoding="utf-8")
    (work / "platform_io.h").write_text(FAKE_PLATFORM_IO, encoding="utf-8")
    text = (mutated or {}).get("drivers/serial.c",
                               (ROOT / "drivers/serial.c").read_text(encoding="utf-8"))
    (work / "serial.c").write_text(text, encoding="utf-8")
    exe = work / "serial-gate-host"
    cmd = ["gcc", *flags_for(mutated), "-I" + str(work), "-I" + str(ROOT / "include"),
           "-I" + str(ROOT / "drivers"), "-I" + str(ROOT / "sdk/include/os32"),
           str(GATE_HARNESS), "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True,
                   stderr=subprocess.DEVNULL if mutated is not None else None)
    return exe


def run_exe_cases(exe, cases, quiet=False, tag=""):
    failed = 0
    for case in cases:
        try:
            rc = subprocess.run([str(exe), case], cwd=ROOT, timeout=60,
                                stdout=subprocess.DEVNULL if quiet else None,
                                stderr=subprocess.DEVNULL if quiet else None
                                ).returncode
        except subprocess.TimeoutExpired:
            rc = 124
        if not quiet:
            print(f"EXIT {tag}{case}={rc}", flush=True)
        failed += rc != 0
    return failed


def build_target(tmp):
    """カーネルと同じ i386-elf で新しい実物を -Werror で通す。"""
    base = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386",
            "-ffreestanding", "-fno-pie", "-fno-stack-protector", "-nostdlib",
            "-mno-red-zone", "-fcommon", "-fsigned-char", "-fno-short-enums",
            "-O2", "-Wall", "-Werror", "-Wdeclaration-after-statement",
            "-D__KERNEL_BUILD__"]
    inc = ["-I.", "-Iinclude", "-Iarch/x86", "-Iplatform/pc98", "-Isdk/include",
           "-Isdk/include/os32", "-Ikernel", "-Idrivers", "-Inet", "-Ifs",
           "-Iexec", "-Igfx", "-Ilib", "-Ikapi"]
    for rel in ("fs/sfs_proto.c", "fs/sfs_client.c", "fs/serialfs.c",
                "fs/serialfs_session.c", "drivers/serial.c",
                "drivers/serial_plan.c"):
        out = pathlib.Path(tmp) / (rel.replace("/", "_") + ".o")
        subprocess.run(base + inc + ["-c", rel, "-o", str(out)], cwd=ROOT,
                       check=True)
    print("TARGET i386-elf GNU89 -Werror PASS (sfs_proto / sfs_client / "
          "serialfs / serialfs_session / serial / serial_plan)", flush=True)


# ============================================================================
#  Python の実物
# ============================================================================
def load_pair(h_path=None, r_path=None):
    """serialfs_host と rshell_serial を (写しでも) 読み込む。rshell_serial の
    `import serialfs_host` が同じものを引くよう sys.modules に置いてから読む。"""
    spec = importlib.util.spec_from_file_location(
        "serialfs_host", str(h_path or TOOLS / "serialfs_host.py"))
    h = importlib.util.module_from_spec(spec)
    sys.modules["serialfs_host"] = h
    spec.loader.exec_module(h)
    spec = importlib.util.spec_from_file_location(
        "rshell_serial_under_test", str(r_path or TOOLS / "rshell_serial.py"))
    r = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(r)
    r.serialfs_host = h
    return h, r


FAILED = []


def check(cond, what):
    if not cond:
        FAILED.append(what)


def frames_of(h, data):
    dm = h.Demux()
    return [e[1] for e in dm.feed(data, 0.0) if e[0] == "frame"]


def case_demux(h, r):
    hello = h.encode(h.T_HELLO, 0, 0, struct.pack("<HHI", 1, 512, 7))
    # 本文に ENQ・EOT・ESC・'SF' を含むフレーム
    tricky = h.encode(h.T_LOG, 9, 0, b"a\x04b\x05SF\x1bc\x04")
    stream = (b"> sfs run x\r\n" + hello + b"mid" + tricky + b"tail\x04after")
    dm = h.Demux()
    ev = dm.feed(stream, 0.0)
    kinds = [e[0] for e in ev]
    check(kinds == ["text", "frame", "text", "frame", "text", "eot", "text"],
          "demux: kinds %r" % kinds)
    check(ev[0][1] == b"> sfs run x\r\n", "demux: leading text")
    check(ev[3][1].payload == b"a\x04b\x05SF\x1bc\x04",
          "demux: EOT / ENQ inside the payload are payload")
    # 1 バイトずつ入れても同じ (途中のフレームは待つ)
    dm = h.Demux()
    ev2 = []
    for i in range(len(stream)):
        ev2 += dm.feed(stream[i:i + 1], 0.0)
    kinds2 = [e[0] for e in ev2 if e[0] != "text"]
    text2 = b"".join(e[1] for e in ev2 if e[0] == "text")
    check(kinds2 == ["frame", "frame", "eot"], "demux: bytewise kinds %r" % kinds2)
    check(text2 == b"> sfs run x\r\nmidtailafter", "demux: bytewise text %r" % text2)
    # CRC の壊れたフレームは**丸ごと捨てる** — 中の 0x04 で行を終えない
    bad = bytearray(h.encode(h.T_LOG, 9, 0, b"xx\x04yy"))
    bad[-1] ^= 0xFF
    dm = h.Demux()
    ev = dm.feed(bytes(bad) + b"z\x04", 0.0)
    check([e[0] for e in ev] == ["text", "eot"] and ev[0][1] == b"z",
          "demux: corrupt frame skipped whole %r" % ev)
    check(dm.bad_frames == 1, "demux: bad frame counted")
    # 印の無い ENQ は文字
    dm = h.Demux()
    ev = dm.feed(b"a\x05b\x05Sx\x04", 0.0)
    check(ev == [("text", b"a\x05b\x05Sx"), ("eot",)], "demux: bare ENQ is text %r" % ev)
    # 長さ欄が上限を越えるヘッダは文字 (フレームを待たない)
    hdr = b"\x05SF" + struct.pack("<BIHH", 0x10, 1, 1, 513)
    dm = h.Demux()
    ev = dm.feed(hdr + b"\x04", 0.0)
    check(ev and ev[-1] == ("eot",), "demux: oversize header does not swallow EOT")
    # 途中で途切れたフレームは、止まってから文字として流す
    dm = h.Demux()
    ev = dm.feed(hello[:8], 0.0)
    check(ev == [], "demux: partial frame waits")
    ev = dm.poll(0.1)
    check(ev == [], "demux: short gap still waits")
    ev = dm.poll(1.0)
    check(ev and ev[0][0] == "text", "demux: stalled partial becomes text")
    ev = dm.feed(b"\x04", 1.0)
    check(("eot",) in ev, "demux: EOT after a stalled partial")


def mk_tree(tmp):
    root = pathlib.Path(tmp) / "hostroot"
    root.mkdir()
    (root / "hello.txt").write_bytes(b"hello from the host\n" * 200)
    (root / "gone.txt").write_bytes(b"x")
    many = root / "many"
    many.mkdir()
    for i in range(150):
        (many / ("file-%03d-with-a-long-name.bin" % i)).write_bytes(b"z" * i)
    (root / "sub").mkdir()
    outside = pathlib.Path(tmp) / "outside"
    outside.mkdir()
    (outside / "secret").write_bytes(b"s")
    os.symlink(str(outside), str(root / "escape"))
    os.symlink(str(root / "hello.txt"), str(root / "inside_link"))
    return root


def case_paths(h, r):
    with tempfile.TemporaryDirectory(prefix="os32-sfs-paths-") as tmp:
        root = mk_tree(tmp)
        fs = h.HostFS(str(root))
        for bad in (b"/../outside/secret", b"/sub/../../outside", b"/.",
                    b"/sub/..", b"relative", b"/a\\b", b"/a\x00b",
                    b"/escape/secret", b"/escape"):
            try:
                fs.resolve(bad)
                check(False, "paths: %r accepted" % bad)
            except h.SfsError as e:
                check(e.code == h.ERR_INVAL, "paths: %r code %d" % (bad, e.code))
        check(fs.resolve(b"/") == fs.root, "paths: root")
        check(fs.resolve(b"//sub//") == os.path.join(fs.root, b"sub"),
              "paths: empty components folded")
        check(fs.resolve(b"/inside_link").endswith(b"inside_link"),
              "paths: symlink inside the root is fine")
        for op in (lambda: fs.unlink(b"/"), lambda: fs.rmdir(b"/"),
                   lambda: fs.rename(b"/", b"/x"), lambda: fs.write(b"/", 0, 0, b"")):
            try:
                op()
                check(False, "paths: root modification accepted")
            except h.SfsError:
                pass
        # 操作の結果
        st, body = fs.stat(b"/hello.txt")
        kind, size, mtime = struct.unpack("<BII", body)
        check(kind == h.KIND_FILE and size == 4000 and mtime > 0, "stat file")
        st, body = fs.stat(b"/many")
        check(struct.unpack("<BII", body)[0] == h.KIND_DIR, "stat dir")
        # 列挙は頁に分かれ、cookie は前へ進み、全部そろう
        names, cookie, pages = [], 0, 0
        while True:
            st, body = fs.list(b"/many", cookie)
            nxt = struct.unpack_from("<I", body)[0]
            check(len(body) + 4 <= h.MAX_PAYLOAD, "list: page fits")
            pos = 4
            while pos < len(body):
                k, sz, nl = struct.unpack_from("<BIB", body, pos)
                names.append(body[pos + 6:pos + 6 + nl])
                pos += 6 + nl
            pages += 1
            if nxt == 0:
                break
            check(nxt > cookie, "list: cookie advances")
            cookie = nxt
        check(len(names) == 150 and names == sorted(names) and pages > 1,
              "list: %d names in %d pages" % (len(names), pages))
        n, data = fs.read(b"/hello.txt", 3990, 100)
        check(n == 10 and data == b"the host\n"[-10:] or n == 10, "read at EOF")
        n, data = fs.read(b"/hello.txt", 0, 5000)
        check(n == h.READ_MAX, "read is clamped to READ_MAX")


def req(h, t, sid, seq, payload):
    f = frames_of(h, h.encode(t, sid, seq, payload))
    return f[0]


def pth(p):
    return bytes([len(p)]) + p


def case_server(h, r):
    with tempfile.TemporaryDirectory(prefix="os32-sfs-server-") as tmp:
        root = mk_tree(tmp)
        sv = h.Server(h.HostFS(str(root)))
        hello = req(h, h.T_HELLO, 0, 0, struct.pack("<HHI", 1, 512, 42))
        r1 = sv.handle(hello)
        sid = frames_of(h, r1)[0].sid
        check(sid != 0, "server: session id")
        # HELLO の再送は同じ ID (応答をそのまま返す)
        check(sv.handle(hello) == r1 and sv.sid == sid, "server: HELLO resend cached")
        # 未知の ID には ERR (STALE)
        e = frames_of(h, sv.handle(req(h, h.T_STAT, sid ^ 1, 1, pth(b"/"))))[0]
        check(e.type == h.T_ERR and struct.unpack("<i", e.payload)[0] == h.ERR_STALE,
              "server: unknown session -> ERR STALE")
        # **再送で副作用が二重にならない**: RENAME / UNLINK / MKDIR / WRITE
        ren = req(h, h.T_RENAME, sid, 1, pth(b"/hello.txt") + pth(b"/renamed.txt"))
        a = sv.handle(ren)
        ex = sv.executed
        b = sv.handle(ren)
        check(a == b and sv.executed == ex and sv.replayed >= 1,
              "server: RENAME resend replayed, not re-executed")
        check(struct.unpack_from("<i", frames_of(h, b)[0].payload)[0] == 0,
              "server: replayed RENAME still says OK")
        check((root / "renamed.txt").exists() and not (root / "hello.txt").exists(),
              "server: rename happened once")
        mk = req(h, h.T_MKDIR, sid, 2, pth(b"/newdir"))
        a = sv.handle(mk)
        b = sv.handle(mk)
        check(a == b and struct.unpack_from("<i", frames_of(h, b)[0].payload)[0] == 0,
              "server: MKDIR resend is not EXIST")
        ul = req(h, h.T_UNLINK, sid, 3, pth(b"/gone.txt"))
        a = sv.handle(ul)
        b = sv.handle(ul)
        check(a == b and struct.unpack_from("<i", frames_of(h, b)[0].payload)[0] == 0,
              "server: UNLINK resend is not NOTFOUND")
        wr = req(h, h.T_WRITE, sid, 4,
                 struct.pack("<IB", 0, h.WF_TRUNC) + pth(b"/w.bin") + b"abc")
        ex = sv.executed
        sv.handle(wr)
        sv.handle(wr)
        check(sv.executed == ex + 1 and (root / "w.bin").read_bytes() == b"abc",
              "server: WRITE resend executed once")
        # 同じ番号でも中身 (CRC) が違えば別の要求
        wr2 = req(h, h.T_WRITE, sid, 4,
                  struct.pack("<IB", 0, h.WF_TRUNC) + pth(b"/w.bin") + b"xyz")
        sv.handle(wr2)
        check((root / "w.bin").read_bytes() == b"xyz", "server: same seq, other CRC executes")
        # TRUNC の無い書き込みは途中から (作り直さない)
        sv.handle(req(h, h.T_WRITE, sid, 9,
                      struct.pack("<IB", 3, 0) + pth(b"/w.bin") + b"123"))
        check((root / "w.bin").read_bytes() == b"xyz123", "server: WRITE without TRUNC appends at offset")
        # キャッシュは直前の 1 件だけ (前の番号の再送は実行し直す)
        ex = sv.executed
        sv.handle(req(h, h.T_STAT, sid, 5, pth(b"/")))
        sv.handle(ul)
        check(sv.executed == ex + 2, "server: cache holds only the last request")
        # BYE の後は何も送らない。LOG / EXIT は受け取る
        sv.handle(req(h, h.T_BYE, sid, 0, b""))
        check(sv.handle(req(h, h.T_STAT, sid, 6, pth(b"/"))) is None,
              "server: nothing after BYE")
        sv.handle(req(h, h.T_LOG, sid, 0, b"log line\n"))
        sv.handle(req(h, h.T_EXIT, sid, 0, struct.pack("<iII", 3, 0, 0)))
        check(bytes(sv.log_text) == b"log line\n" and sv.exit == (3, 0, 0),
              "server: LOG / EXIT after BYE")
        # 新しいセッション。古い ID には答えない
        h2 = req(h, h.T_HELLO, 0, 0, struct.pack("<HHI", 1, 512, 43))
        sid2 = frames_of(h, sv.handle(h2))[0].sid
        check(sid2 not in (0, sid), "server: new session")
        check(sv.handle(req(h, h.T_STAT, sid, 7, pth(b"/"))) is None,
              "server: closed session stays silent")
        # 壊れたペイロードでも落ちずに status で答える
        rr = frames_of(h, sv.handle(req(h, h.T_READ, sid2, 1, b"\x01")))[0]
        check(struct.unpack_from("<i", rr.payload)[0] == h.ERR_INVAL,
              "server: short payload -> INVAL")
        # `..` はワイヤでも断る
        rr = frames_of(h, sv.handle(req(h, h.T_STAT, sid2, 2, pth(b"/../x"))))[0]
        check(struct.unpack_from("<i", rr.payload)[0] == h.ERR_INVAL,
              "server: .. rejected on the wire")


class GuestPort(object):
    """rshell_serial が話す相手の偽物。書かれた応答を見て次の要求を出す。

    script(h, port, frame) は応答 1 つごとに呼ばれ、次に送るバイトを返す。
    """

    def __init__(self, h, first, script, clock):
        self.h = h
        self.q = bytearray()
        self.writes = []
        self.first = first
        self.script = script
        self.clock = clock
        self.dm = h.Demux()

    def reset_input_buffer(self):
        pass

    def flush(self):
        pass

    def write(self, data):
        self.writes.append(bytes(data))
        if len(self.writes) == 1:
            self.q += self.first(data)
            return
        for e in self.dm.feed(data, 0.0):
            if e[0] == "frame":
                self.q += self.script(e[1])

    def read(self, n):
        if not self.q:
            self.clock.t += 0.01
            return b""
        out = bytes(self.q[:n])
        del self.q[:n]
        return out


class Clock(object):
    def __init__(self, step=0.0):
        self.t = 0.0
        self.step = step

    def __call__(self):
        self.t += self.step
        return self.t


def guest_script(h, log=b"hsync: ok\nsfs: exit=0\n", code=0):
    state = {"sid": 0}

    def first(line):
        return (b"> " + line.rstrip(b"\n") + b"\n" +
                h.encode(h.T_HELLO, 0, 0, struct.pack("<HHI", 1, 512, 99)))

    def step(fr):
        if fr.type == (h.T_HELLO | h.T_RESP):
            state["sid"] = fr.sid
            return h.encode(h.T_STAT, fr.sid, 1, pth(b"/hello.txt"))
        if fr.type == (h.T_STAT | h.T_RESP):
            sid = state["sid"]
            return (h.encode(h.T_BYE, sid, 0) + h.encode(h.T_LOG, sid, 0, log) +
                    h.encode(h.T_EXIT, sid, 0, struct.pack("<iII", code, 0, 0)) +
                    b"\x04")
        return b""
    return first, step


class Args(object):
    timeout = 30.0


def case_serve_line(h, r):
    with tempfile.TemporaryDirectory(prefix="os32-sfs-serve-") as tmp:
        root = mk_tree(tmp)
        # 正常: HELLO → STAT → BYE / LOG / EXIT → EOT
        clock = Clock()
        first, step = guest_script(h)
        port = GuestPort(h, first, step, clock)
        sv = h.Server(h.HostFS(str(root)), now=clock)
        out = io.StringIO()
        res = h.serve_line(port, "sfs run hsync boot", sv, 30.0, out,
                           now=clock, sleep=lambda s: None)
        check(res["eot"] and res["exit"] == (0, 0, 0), "serve: exit frame %r" % res)
        check(res["sent"] == 2, "serve: two responses sent")
        check("> sfs run hsync boot" in out.getvalue() and
              "hsync: ok" in out.getvalue(), "serve: echo + log shown")
        check(sv.sid == 0, "serve: session closed at EOT")
        # run_line (rshell_serial) の結果: 子が 0 なら 0
        clock = Clock()
        first, step = guest_script(h, code=2)
        port = GuestPort(h, first, step, clock)
        sv = h.Server(h.HostFS(str(root)), now=clock)
        saved = h.serve_line
        h.serve_line = (lambda p, l, s, t, o: saved(p, l, s, t, o, now=clock,
                                                    sleep=lambda x: None))
        try:
            rc, text = r.run_line(Args(), port, "sfs run hsync boot", sv,
                                  out=io.StringIO())
        finally:
            h.serve_line = saved
        check(rc == 1, "run_line: nonzero child -> rc 1")
        # 期限切れの応答は送らない (処理に 2 秒以上かかった)
        clock = Clock(step=2.5)
        first, step = guest_script(h)
        gave_up = {"n": 0}

        def first2(line):
            gave_up["n"] += 1
            return first(line)
        port = GuestPort(h, first2, step, clock)
        port.q += b""
        sv = h.Server(h.HostFS(str(root)), now=clock)
        orig_read = port.read

        def read_then_eot(n):
            d = orig_read(n)
            if not d and len(port.writes) == 1 and clock.t > 20:
                return b"\x04"          # ゲストはあきらめて EOT
            return d
        port.read = read_then_eot
        res = h.serve_line(port, "sfs run x", sv, 1000.0, io.StringIO(),
                           now=clock, sleep=lambda s: None)
        check(res["late"] >= 1 and res["sent"] == 0 and len(port.writes) == 1,
              "serve: late response not sent %r" % res)


def case_outside_session(h, r):
    """`sfs run` の行の外ではフレームを解釈しない (cat の本文に ENQ / EOT)。"""
    with tempfile.TemporaryDirectory(prefix="os32-sfs-outside-") as tmp:
        root = mk_tree(tmp)
        sv = h.Server(h.HostFS(str(root)))
        hello = h.encode(h.T_HELLO, 0, 0, struct.pack("<HHI", 1, 512, 5))
        stat = h.encode(h.T_STAT, 0x1234, 1, pth(b"/hello.txt"))

        class CatPort(object):
            def __init__(self):
                self.writes = []
                self.q = bytearray()

            def reset_input_buffer(self):
                pass

            def flush(self):
                pass

            def write(self, d):
                self.writes.append(bytes(d))
                if len(self.writes) == 1:
                    self.q += b"> cat f\n" + hello + stat + b"body\x04"

            def read(self, n):
                d = bytes(self.q[:n])
                del self.q[:n]
                return d
        port = CatPort()
        rc, text = r.run_line(Args(), port, "cat f", sv, out=io.StringIO())
        check(len(port.writes) == 1, "outside: host answered a frame in a cat body")
        check(sv.executed == 0 and sv.sid == 0, "outside: server untouched")
        check(rc == 0 and text.startswith("> cat f"), "outside: normal reply")
        # 行の判定は rsh_sfs_child と同じ
        for line, want in (("sfs run hsync boot", "hsync boot"),
                           ("  sfs\trun  ls /host", "ls /host"),
                           ("sfs run", None), ("sfsrun x", None),
                           ("echo x && sfs run y", None), ("sfs runx y", None)):
            check(h.sfs_child(line) == want, "sfs_child(%r)" % line)


def case_aidebug_port(h, r):
    """NP21/W の COM1 を HTTP で読み書きする口 (T2 用、ini は変えない)。"""
    import json

    class Resp(object):
        def __init__(self, body):
            self.body = body

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self):
            return self.body
    state = {"tx": [b"\x05SF" + b"a" * 10, b"", b"\x04"], "written": b""}

    def urlopen(req, timeout=0):
        url = req if isinstance(req, str) else req.full_url
        if url.endswith("/api/serial/read"):
            chunk = state["tx"].pop(0) if state["tx"] else b""
            return Resp(json.dumps({"ok": True, "len": len(chunk),
                                    "hex": chunk.hex()}).encode())
        if url.endswith("/api/serial/write"):
            state["written"] += bytes.fromhex(req.data.decode())
            return Resp(b'{"ok":true,"written":1}')
        raise AssertionError(url)
    port = r.AidebugPort("http://x:8025/", timeout=0.05, urlopen=urlopen)
    check(port.read(4) == b"\x05SFa", "aidebug: read splits a chunk")
    check(port.read(100) == b"a" * 9, "aidebug: rest of the chunk")
    check(port.read(10) == b"\x04", "aidebug: keeps polling past an empty chunk")
    check(port.read(10) == b"", "aidebug: nothing within the timeout")
    port.write(b"\x05SF\x00\xff")
    check(state["written"] == b"\x05SF\x00\xff", "aidebug: write hex")
    state["tx"] = [b"old", b"older", b""]
    port.reset_input_buffer()
    check(state["tx"] == [] and port.read(1) == b"", "aidebug: reset drains")
    check(isinstance(r.open_port("aidebug:http://127.0.0.1:8025", 9600),
                     r.AidebugPort), "aidebug: open_port prefix")


PY_CASES = {
    "aidebug_port": case_aidebug_port,
    "demux": case_demux,
    "paths": case_paths,
    "server": case_server,
    "serve_line": case_serve_line,
    "outside_session": case_outside_session,
}


def run_py(h, r, names, quiet=False):
    global FAILED
    bad = 0
    for name in names:
        FAILED = []
        try:
            PY_CASES[name](h, r)
        except Exception as e:  # noqa: BLE001
            FAILED.append("raised %r" % (e,))
        rc = 1 if FAILED else 0
        if not quiet:
            for f in FAILED:
                print(f"  FAIL {name}: {f}", flush=True)
            print(f"EXIT py:{name}={rc}", flush=True)
        bad += rc
    return bad


# ============================================================================
#  C ⇔ Python の照合と結合
# ============================================================================
def cross_check(exe, h):
    bad = 0
    out = subprocess.run([str(exe), "vectors"], capture_output=True, text=True,
                         check=True).stdout.split()
    pl = bytes((i * 31 + 7) & 0xFF for i in range(512))
    want = [h.encode(h.T_HELLO, 0, 0, pl[:8]),
            h.encode(h.T_READ, 0xDEADBEEF, 1, b""),
            h.encode(h.T_WRITE | h.T_RESP, 0x01020304, 0xFFFE, pl),
            h.encode(h.T_ERR, 0x80000001, 0x8000, pl[:4])]
    for i, w in enumerate(want):
        if bytes.fromhex(out[i]) != w:
            print(f"  FAIL xc: C frame {i} != Python frame", flush=True)
            bad += 1
    # Python の列を C の受信器で読む (ごみ・壊れたフレームを挟む)
    bad_frame = bytearray(want[1])
    bad_frame[-2] ^= 1
    stream = b"\x41\x05" + want[2] + bytes(bad_frame) + want[3]
    res = subprocess.run([str(exe), "decode"], input=stream.hex() + "\n",
                         capture_output=True, text=True, check=True).stdout
    lines = res.strip().split("\n")
    if lines[-1] != "bad_crc=1 bad_len=0 frames=2":
        print(f"  FAIL xc: C decode summary {lines[-1]!r}", flush=True)
        bad += 1
    if not lines[0].startswith("93 01020304 fffe 512 " + pl[:4].hex()):
        print(f"  FAIL xc: C decode frame {lines[0][:40]!r}", flush=True)
        bad += 1
    print(f"EXIT xc:frames={int(bool(bad))}", flush=True)
    return bad


def header_consts(text, names):
    got = {}
    for n in names:
        m = re.search(r"#define\s+%s\s+\(?([0-9A-Fa-fx]+)" % n, text)
        got[n] = int(m.group(1).rstrip("UL"), 0) if m else None
    return got


def const_check(h):
    bad = 0
    proto = (ROOT / "fs/sfs_proto.h").read_text(encoding="utf-8")
    pairs = {"SFS_ENQ": h.ENQ, "SFS_VERSION": h.VERSION, "SFS_HDR_LEN": h.HDR_LEN,
             "SFS_CRC_LEN": h.CRC_LEN, "SFS_MAX_PAYLOAD": h.MAX_PAYLOAD,
             "SFS_T_HELLO": h.T_HELLO, "SFS_T_BYE": h.T_BYE,
             "SFS_T_STAT": h.T_STAT, "SFS_T_LIST": h.T_LIST,
             "SFS_T_READ": h.T_READ, "SFS_T_WRITE": h.T_WRITE,
             "SFS_T_MKDIR": h.T_MKDIR, "SFS_T_RMDIR": h.T_RMDIR,
             "SFS_T_UNLINK": h.T_UNLINK, "SFS_T_RENAME": h.T_RENAME,
             "SFS_T_LOG": h.T_LOG, "SFS_T_EXIT": h.T_EXIT,
             "SFS_T_RESP": h.T_RESP, "SFS_T_ERR": h.T_ERR,
             "SFS_WF_TRUNC": h.WF_TRUNC, "SFS_KIND_FILE": h.KIND_FILE,
             "SFS_KIND_DIR": h.KIND_DIR, "SFS_KIND_OTHER": h.KIND_OTHER,
             "SFS_XF_NOT_QUIET": h.XF_NOT_QUIET, "SFS_XF_DEAD": h.XF_DEAD,
             "SFS_PATH_MAX": h.PATH_MAX, "SFS_FIRST_BYTE_MS": int(h.FIRST_BYTE_S * 1000),
             "SFS_GAP_MS": int(h.GAP_S * 1000)}
    got = header_consts(proto, pairs)
    for n, v in pairs.items():
        if got[n] != v:
            print(f"  FAIL const: {n} C={got[n]} Python={v}", flush=True)
            bad += 1
    shared = (ROOT / "sdk/include/os32/os32_kapi_shared.h").read_text(encoding="utf-8")
    for n, v in (("OS32_ERR_IO", h.ERR_IO), ("OS32_ERR_NOTFOUND", h.ERR_NOTFOUND),
                 ("OS32_ERR_EXIST", h.ERR_EXIST), ("OS32_ERR_NOTDIR", h.ERR_NOTDIR),
                 ("OS32_ERR_NOTEMPTY", h.ERR_NOTEMPTY), ("OS32_ERR_ISDIR", h.ERR_ISDIR),
                 ("OS32_ERR_INVAL", h.ERR_INVAL), ("OS32_ERR_STALE", h.ERR_STALE),
                 ("OS32_ERR_NOSPC", h.ERR_NOSPC),
                 ("OS32_ERR_NAMETOOLONG", h.ERR_NAMETOOLONG)):
        m = re.search(r"#define\s+%s\s+(-?\d+)" % n, shared)
        if not m or int(m.group(1)) != v:
            print(f"  FAIL const: {n}", flush=True)
            bad += 1
    boot = (ROOT / "boot/boot_defs.h").read_text(encoding="utf-8")
    inc = (ROOT / "userland/system/hsync_bootold.inc").read_text(encoding="utf-8")
    for a, b in (("VK32_MAGIC", "HBO_VK32_MAGIC"), ("VK32_VERSION", "HBO_VK32_VERSION"),
                 ("VK32_MAX_ENTRIES", "HBO_VK32_MAX_ENTRIES"),
                 ("VK32_COMMON_SIZE", "HBO_VK32_COMMON_SIZE"),
                 ("VK32_ENTRY_SIZE", "HBO_VK32_ENTRY_SIZE")):
        va = header_consts(boot, [a])[a]
        vb = header_consts(inc, [b])[b]
        if va is None or va != vb:
            print(f"  FAIL const: {a}={va} vs {b}={vb}", flush=True)
            bad += 1
    # image_crc の位置の式が boot_defs.h と同じ (16 + 20n + 4)
    if "VK32_OFF_IMAGE_CRC(n)  (VK32_OFF_IMAGE_SIZE(n) + 4UL)" not in boot or \
            "(VK32_COMMON_SIZE + (u32)(n) * (VK32_ENTRY_SIZE + 4UL))" not in boot:
        print("  FAIL const: boot_defs.h の image_crc の位置の式が変わった", flush=True)
        bad += 1
    print(f"EXIT xc:consts={int(bool(bad))}", flush=True)
    return bad


def integration(exe, h):
    """C の受け手 (実物) と Python のホスト (実物) を実時間のパイプでつなぐ。

    RENAME の 1 回目の応答を落とし (ゲストは同じ番号で再送 → ホストは
    **実行し直さずに**保存した応答を返す。実行し直すと NOTFOUND になる)、
    READ の 1 回目の応答の CRC を壊す。
    """
    import time
    bad = []
    with tempfile.TemporaryDirectory(prefix="os32-sfs-int-") as tmp:
        root = mk_tree(tmp)
        (root / "hello.txt").write_bytes(bytes(range(256)) * 20)
        sv = h.Server(h.HostFS(str(root)))
        p = subprocess.Popen([str(exe), "pipe"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.set_blocking(p.stdout.fileno(), False)
        dm = h.Demux()
        drop = {h.T_RENAME: 1}
        corrupt = {h.T_READ: 1}
        dropped = corrupted = 0
        t_end = time.monotonic() + 60
        while p.poll() is None and time.monotonic() < t_end:
            rd, _, _ = select.select([p.stdout], [], [], 0.05)
            data = b""
            if rd:
                try:
                    data = os.read(p.stdout.fileno(), 4096)
                except BlockingIOError:
                    data = b""
            for e in dm.feed(data, time.monotonic()):
                if e[0] != "frame":
                    continue
                resp = sv.handle(e[1])
                if resp is None:
                    continue
                t = e[1].type
                if drop.get(t):
                    drop[t] -= 1
                    dropped += 1
                    continue
                if corrupt.get(t):
                    corrupt[t] -= 1
                    corrupted += 1
                    resp = bytearray(resp)
                    resp[-1] ^= 0x55
                    resp = bytes(resp)
                try:
                    p.stdin.write(resp)
                    p.stdin.flush()
                except BrokenPipeError:
                    break
        if p.poll() is None:
            p.kill()
            bad.append("child did not finish")
        err = p.stderr.read().decode()
        vals = dict()
        for line in err.splitlines():
            parts = line.split()
            if parts:
                vals[parts[0]] = dict(kv.split("=", 1) for kv in parts[1:] if "=" in kv)
        hello = (root / "hello.txt").read_bytes()
        exp = {
            ("stat", "rc"): "0", ("stat", "size"): str(len(hello)),
            ("read", "rc"): str(len(hello)),
            ("read", "crc"): "%08x" % (zlib.crc32(hello) & 0xFFFFFFFF),
            ("list", "rc"): "0", ("list", "n"): "150",
            ("write", "rc"): "3000", ("rename", "rc"): "0",
            ("mkdir", "rc"): "0", ("unlink", "rc"): "0",
            ("escape", "rc"): str(h.ERR_INVAL), ("nope", "rc"): str(h.ERR_NOTFOUND),
        }
        for (k, f), v in exp.items():
            got = vals.get(k, {}).get(f)
            if got != v:
                bad.append("%s.%s = %r (want %r)" % (k, f, got, v))
        if (root / "out.bin").exists() or not (root / "out2.bin").exists():
            bad.append("rename not applied exactly once")
        elif (root / "out2.bin").read_bytes() != bytes((i * 3) & 0xFF for i in range(3000)):
            bad.append("written content differs")
        if not (root / "newdir").is_dir() or (root / "gone.txt").exists():
            bad.append("mkdir / unlink not applied")
        if dropped != 1 or corrupted != 1 or sv.replayed < 1:
            bad.append("faults: dropped=%d corrupted=%d replayed=%d" %
                       (dropped, corrupted, sv.replayed))
        st = vals.get("stats", {})
        if int(st.get("resends", 0)) < 2 or int(st.get("bad_crc", 0)) < 1:
            bad.append("guest stats %r" % st)
    for b in bad:
        print(f"  FAIL xc:integration: {b}", flush=True)
    print(f"EXIT xc:integration={int(bool(bad))}", flush=True)
    return len(bad)


# ============================================================================
#  変異
# ============================================================================
IDENTITY = "IDENTITY"
C_MUTATIONS = [
    ("fs/sfs_proto.c", r"(    /\* CRC は種別 \(off 3\) からペイロードの末尾まで)",
     r"\1", IDENTITY),
    ("fs/sfs_proto.c", r"crc = sfs_crc32\(out \+ 3, \(u32\)\(SFS_HDR_LEN - 3 \+ len\)\);",
     "crc = sfs_crc32(out + 4, (u32)(SFS_HDR_LEN - 4 + len));",
     "CRC の範囲から種別を外す (Python と食い違う)"),
    ("fs/sfs_proto.c", r"        if \(len > SFS_MAX_PAYLOAD\) \{\n            d->bad_len\+\+;",
     "        if (len > 0xFFFF) {\n            d->bad_len++;",
     "受信器が長さ欄を上限と照合しない"),
    ("fs/sfs_proto.c", r"    if \(len > SFS_MAX_PAYLOAD\) return 0;\n    out\[0\]",
     "    out[0]", "組み立てが上限を越えるペイロードを断らない"),
    ("fs/sfs_proto.c", r"if \(\*seq >= SFS_SEQ_LAST\) return -1;",
     "if (*seq >= 0xFFFFu) *seq = 0;", "番号を周回させる"),
    ("fs/sfs_proto.c", r"\(u32\)SFS_FIRST_BYTE_MS \+ frame_ms \+ \(u32\)SFS_SLACK_MS",
     "(u32)SFS_SLACK_MS", "最初のバイトまでの 2 秒を期限に入れない"),
    ("fs/sfs_client.c", r"            if \(sfs_match\(c, &f, type, seq, hello, nonce\)\) \{\n"
                        r"                \*got = f;\n                return 0;\n            \}\n"
                        r"            c->stray\+\+;",
     "            if (sfs_match(c, &f, type, seq, hello, nonce)) {\n"
     "                *got = f;\n                return 0;\n            }\n"
     "            c->stray++;\n            start = t;",
     "受け取らなかったフレームで期限を延ばす"),
    ("fs/sfs_client.c", r"    if \(f->sid != c->sid \|\| f->seq != seq\) return 0;",
     "    if (f->sid != c->sid) return 0;", "番号違いの応答を取る"),
    ("fs/sfs_client.c", r"    if \(f->sid != c->sid \|\| f->seq != seq\) return 0;",
     "    if (f->seq != seq) return 0;", "セッション違いの応答を取る"),
    ("fs/sfs_client.c", r"        if \(sfs_get32\(f->payload \+ 8\) != nonce\) return 0;\n",
     "", "HELLO の nonce を見ない (前のセッションの遅れた HELLO を取る)"),
    ("fs/sfs_client.c", r"    if \(c->fails >= SFS_DEAD_AFTER && c->dead == SFS_DEAD_NONE\)",
     "    if (0)", "連続失敗で線を死んだと見なさない"),
    ("fs/sfs_client.c", r"        c->dead = SFS_DEAD_STALE;\n", "",
     "ERR (未知のセッション) で止まらない"),
    ("fs/sfs_client.c", r"    for \(attempt = 0; attempt <= SFS_RETRIES; attempt\+\+\) \{",
     "    for (attempt = 0; attempt < 1; attempt++) {", "再送しない"),
    ("fs/sfs_client.c", r"        if \(c->io->put\(c->ctx, c->tx, flen\) != 0\) \{\n            c->timeouts\+\+;",
     "        if (attempt > 0) sfs_encode(c->tx, type, c->sid, (u16)(seq + attempt), c->tx + SFS_HDR_LEN, (u16)(flen - SFS_HDR_LEN - SFS_CRC_LEN));\n"
     "        if (c->io->put(c->ctx, c->tx, flen) != 0) {\n            c->timeouts++;",
     "再送で番号を変える (ホストのキャッシュが効かず副作用が二重になる)"),
    ("fs/sfs_client.c", r"    if \(!c->io->can_wait\(c->ctx\)\) return OS32_ERR_IO;\n    /\* 番号は",
     "    /* 番号は", "IF=0 でも待つ"),
    ("fs/sfs_client.c", r"        if \(\(u32\)\(t - start\) >= limit\) return 0;",
     "", "静まるのを待つ口に上限が無い"),
    ("fs/serialfs.c", r"    if \(!g_permit_cli \|\| g_mounted\) return \(void \*\)0;",
     "    if (g_mounted) return (void *)0;", "セッションの外からマウントできる"),
    ("fs/serialfs.c", r"    if \(VFS_MOUNT_DEV_TYPE\(dev\) != VFS_DEV_SERIAL\) return \(void \*\)0;",
     "", "デバイス種別を見ない"),
    ("fs/serialfs.c", r"if \(\(u32\)rc > want \|\| \(u32\)rc != blen\) return OS32_ERR_IO;",
     "if ((u32)rc != blen) return OS32_ERR_IO;", "要求より多く返す応答を通す"),
    ("fs/serialfs.c", r"        if \(\(u16\)rc < want\) break;          /\* 短い = EOF \*/",
     "", "短い応答を EOF と見ない"),
    ("fs/serialfs.c", r"                if \(ch == 0 \|\| ch == '/'\) return OS32_ERR_IO;",
     "", "名前の '/' を通す"),
    ("fs/serialfs.c", r"        if \(next <= cookie\) return OS32_ERR_IO;",
     "", "cookie が進まない相手で回り続ける"),
    ("fs/serialfs.c", r"    if \(kind == SFS_KIND_DIR\) return OS32_ERR_ISDIR;",
     "", "get_file_size がディレクトリにサイズを返す"),
    ("fs/serialfs.c", r"        pl\[4\] = \(u8\)\(\(first && first_trunc\) \? SFS_WF_TRUNC : 0\);",
     "        pl[4] = (u8)(first_trunc ? SFS_WF_TRUNC : 0);",
     "write_file の 2 回目以降も TRUNC (前の塊を消す)"),
    ("fs/serialfs.c", r"    if \(size == 0 && !first_trunc\) return 0;\n", "",
     "0 バイトの write_stream が要求を出す"),
    ("fs/serialfs.c", r"    default:\n        return OS32_ERR_IO;\n    \}\n\}",
     "    default:\n        return st;\n    }\n}", "知らない status を畳まない"),
    ("userland/shell/serial_watchdog.c",
     r"    if \(at_line_start && !followed\) return RSH_ESC_EXIT;",
     "    if (at_line_start) return RSH_ESC_EXIT;",
     "行頭の ESC は後ろに続きがあっても閉じる"),
    ("userland/shell/serial_watchdog.c",
     r"    if \(at_line_start && !followed\) return RSH_ESC_EXIT;",
     "    return RSH_ESC_EXIT;", "シリアルの ESC はどこでも閉じる (旧挙動)"),
    ("userland/shell/serial_watchdog.c",
     r"    if \(p\[0\] != 'r' \|\| p\[1\] != 'u' \|\| p\[2\] != 'n' \|\| !rsh_is_space\(p\[3\]\)\)",
     "    if (p[0] != 'r' || p[1] != 'u' || p[2] != 'n')",
     "`sfs runx` を受ける"),
    ("userland/system/hsync_bootold.inc",
     r"\*off = HBO_VK32_COMMON_SIZE \+ cnt \* \(HBO_VK32_ENTRY_SIZE \+ 4UL\) \+ 4UL;",
     "*off = HBO_VK32_COMMON_SIZE + cnt * HBO_VK32_ENTRY_SIZE + 4UL;",
     "image_crc の位置をエントリの CRC 表抜きで数える"),
    ("userland/system/hsync_bootold.inc",
     r"    if \(booted_crc != disk_crc\) return HBO_REFUSE_DIFF;\n", "",
     "起動していない版でも .old を作る"),
    ("userland/system/hsync_bootold.inc",
     r"    if \(!have_info\) return HBO_REFUSE_INFO;\n", "",
     "起動したイメージの記録が無くても .old を作る"),
    ("userland/system/hsync_bootold.inc",
     r"            if \(k > len - i\) k = len - i;\n            state = crc32_core_update\(state, zero4, k\);",
     "            if (k > len - i) k = len - i;\n            state = crc32_core_update(state, chunk + i, k);",
     "image_crc 欄を 0 にせずに CRC を取る"),
]
GATE_MUTATIONS = [
    ("drivers/serial.c", r"    if \(s_gate\) \{\n        ser_hold_push\(\(u8\)c\);",
     "    if (0) {\n        ser_hold_push((u8)c);",
     "ゲート中の serial_putchar が線へ出る (console の複写がフレームに混ざる)"),
    ("drivers/serial.c", r"    if \(s_gate\) return -1;         /\* rshell / kbd.c / KAPI にフレームを渡さない \*/\n",
     "", "ゲート中の serial_trygetchar が受信を渡す (rshell にフレームが漏れる)"),
    ("drivers/serial.c", r"1 バイトも渡さない\)。 \*/\n    ser_head = 0;\n    ser_tail = 0;\n    ser_count = 0;\n",
     "1 バイトも渡さない)。 */\n    if (on) { ser_head = 0; ser_tail = 0; ser_count = 0; }\n",
     "下ろすときに受信リングを空にしない (遅れた応答が rshell に入る)"),
    ("drivers/serial.c", r"        s_hold_head = \(s_hold_head \+ 1\) % \(u32\)SER_HOLD_SIZE;\n        s_hold_count--;\n        s_hold_dropped\+\+;\n    \}\n    s_hold\[",
     "        s_hold_dropped++;\n        irq_restore(f);\n        return;\n    }\n    s_hold[",
     "保留リングが溢れたら新しい方を捨てる (結果行と exit が消える)"),
    ("drivers/serial.c", r"    if \(s_gate\) \{\n        kprintf\(0x0E, \"\[ser\] refuse serial_init during SerialFS session\\n\"\);\n        return;\n    \}",
     "", "セッション中に速度を変えられる"),
    ("drivers/serial.c", r"            if \(sts & serial_oe_mask\(s_setup.mode\)\) ser_err_oe\+\+;\n            if \(sts & serial_fe_mask",
     "            if (sts & serial_fe_mask", "ISR が OE を数えない"),
]
PY_MUTATIONS = [
    ("tools/serialfs_host.py", r"(    def feed\(self, data, now\):)", r"\1", IDENTITY),
    ("tools/serialfs_host.py", r"            if r\[0\] == \"bad\":\n                self.bad_frames \+= 1\n",
     "            if r[0] == \"bad\":\n                self.bad_frames += 1\n                i += 1\n                continue\n",
     "CRC の壊れたフレームを文字として読み直す (中の 0x04 で行が終わる)"),
    ("tools/serialfs_host.py", r"        if key == self.cache_key:\n            self.replayed \+= 1\n            return self.cache_resp\n",
     "", "応答のキャッシュが無い (再送で RENAME / MKDIR / UNLINK が二重に走る)"),
    ("tools/serialfs_host.py", r"        key = \(fr.sid, fr.seq, fr.crc\)\n        if key == self.cache_key:",
     "        key = (fr.sid, fr.seq)\n        if key == self.cache_key:",
     "キャッシュの一致に要求の CRC を見ない"),
    ("tools/serialfs_host.py", r"            if c in \(b\"\.\", b\"\.\.\"\) or",
     "            if c in (b\".\",) or", "`..` を通す"),
    ("tools/serialfs_host.py", r"        if real != self.root and not real.startswith\(self.root \+ b\"/\"\):\n            raise SfsError\(ERR_INVAL\)",
     "        pass", "realpath で根の外を断らない (symlink で脱出)"),
    ("tools/serialfs_host.py", r"                    if now\(\) - t > FIRST_BYTE_S:",
     "                    if False:", "期限を過ぎた応答も送る"),
    ("tools/serialfs_host.py", r"        if fr.sid in self.closed:\n            return None ",
     "        if False:\n            return None ", "BYE の後も答える"),
    ("tools/serialfs_host.py", r"        if fr.sid == 0 or fr.sid != self.sid:\n            self.stale \+= 1\n",
     "        if False:\n            self.stale += 1\n", "未知のセッションに普通に答える"),
    ("tools/serialfs_host.py", r"        if flags & WF_TRUNC:\n            mode = \"wb\"",
     "        if True:\n            mode = \"wb\"", "TRUNC の無い書き込みでも作り直す"),
    ("tools/rshell_serial.py", r"            and serialfs_host.sfs_child\(line\) is not None\):",
     "):", "`sfs run` の行の外でもフレームを解釈する"),
    ("tools/serialfs_host.py", r"    if not s.startswith\(\"run\"\) or s\[3:4\] not in \(\" \", \"\\t\"\):",
     "    if not s.startswith(\"run\"):", "ホスト側の行の判定が `sfs runx` を受ける"),
]


def mutate(tmp):
    bad = 0
    errors = 0
    for i, (rel, pattern, repl, why) in enumerate(C_MUTATIONS + GATE_MUTATIONS, 1):
        original = (ROOT / rel).read_text(encoding="utf-8")
        text, n = re.subn(pattern, repl, original, count=1)
        if n != 1:
            print(f"MUTATION C{i} ERROR (not applicable): {why}", flush=True)
            errors += 1
            continue
        gate = rel == "drivers/serial.c"
        try:
            exe = (gate_build if gate else c_build)(tmp, {rel: text}, name="mut")
        except subprocess.CalledProcessError:
            print(f"MUTATION C{i} ERROR (compile): {why}", flush=True)
            errors += 1
            continue
        hits = run_exe_cases(exe, GATE_CASES if gate else C_CASES, quiet=True)
        if why == IDENTITY:
            status = "GREEN (control)" if not hits else "**RED (control broken)**"
            bad += bool(hits)
        else:
            status = "RED" if hits else "**GREEN (見逃し)**"
            bad += not hits
        print(f"MUTATION C{i} {status} ({hits} 件): {why}", flush=True)

    for i, (rel, pattern, repl, why) in enumerate(PY_MUTATIONS, 1):
        mdir = pathlib.Path(tmp) / ("pymut%d" % i)
        mdir.mkdir(parents=True, exist_ok=True)
        files = {"tools/serialfs_host.py": mdir / "serialfs_host.py",
                 "tools/rshell_serial.py": mdir / "rshell_serial.py"}
        for src, dst in files.items():
            shutil.copy(ROOT / src, dst)
        original = (ROOT / rel).read_text(encoding="utf-8")
        text, n = re.subn(pattern, repl, original, count=1)
        if n != 1:
            print(f"MUTATION P{i} ERROR (not applicable): {why}", flush=True)
            errors += 1
            continue
        files[rel].write_text(text, encoding="utf-8")
        try:
            h, r = load_pair(files["tools/serialfs_host.py"],
                             files["tools/rshell_serial.py"])
        except Exception:  # noqa: BLE001
            print(f"MUTATION P{i} ERROR (import): {why}", flush=True)
            errors += 1
            continue
        hits = run_py(h, r, list(PY_CASES), quiet=True)
        if why == IDENTITY:
            status = "GREEN (control)" if not hits else "**RED (control broken)**"
            bad += bool(hits)
        else:
            status = "RED" if hits else "**GREEN (見逃し)**"
            bad += not hits
        print(f"MUTATION P{i} {status} ({hits} 件): {why}", flush=True)
    load_pair()                 # 実物へ戻す
    total = len(C_MUTATIONS) + len(GATE_MUTATIONS) + len(PY_MUTATIONS)
    print(f"MUTATION SUMMARY total={total} errors={errors} missed={bad}",
          flush=True)
    return bad + errors


def main():
    args = sys.argv[1:]
    only = [a for a in args if not a.startswith("--")]
    rc = 0
    with tempfile.TemporaryDirectory(prefix="os32-serialfs-") as tmp:
        exe = c_build(tmp)
        gexe = gate_build(tmp)
        print("HOST GNU89 -Werror compile PASS (real fs/sfs_*.c, fs/serialfs.c, "
              "drivers/serial.c)", flush=True)
        if "--target" in args:
            build_target(tmp)
        rc += run_exe_cases(exe, [c for c in C_CASES if not only or c in only])
        rc += run_exe_cases(gexe, [c for c in GATE_CASES if not only or c in only],
                            tag="gate:")
        h, r = load_pair()
        rc += run_py(h, r, [c for c in PY_CASES if not only or c in only])
        if not only:
            rc += cross_check(exe, h)
            rc += const_check(h)
            rc += integration(exe, h)
        if "--mutate" in args:
            rc += mutate(tmp)
    print(f"RESULT {'PASS' if rc == 0 else 'FAIL'} ({rc})", flush=True)
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
