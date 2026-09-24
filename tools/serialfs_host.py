#!/usr/bin/env python3
"""serialfs_host.py — SerialFS のホスト側 (票 TASK_SERIAL_HOSTFS 部品 B)。

ゲストの `sfs run <コマンド行>` (常駐シェルの組込み) が、同じシリアル線の上で
SerialFS のセッションを開く。ホストは `tools/rshell_serial.py --serve-host <dir>`
から、この模組の `serve_line` を使って

  1. `sfs run ...` の 1 行を rshell へ送る
  2. **その行の EOT まで**、線を次のように振り分ける
       - `ENQ 'S' 'F'` で始まり長さと CRC32 が合うもの → フレーム
       - EOT (フレームの外)                           → 行の終わり
       - それ以外                                      → rshell の文字
  3. 要求フレームには `<dir>` を根にした応答を返す。LOG フレームは溜めた
     出力として表示し、EXIT フレームで終了コードを受け取る

形式の正典は fs/sfs_proto.h (C)。ここはその写しで、tools/tests/test_serialfs.py が
C の組み立てた列をここで読み、その逆も確かめる。

規則 (票 §1-v2 B-7' / §1-v3):
  - **`sfs run` の行の外ではフレームを解釈しない** (セッション外の `cat` の本文で
    要求が動かない)。この模組は serve_line の中でしか使われない
  - 応答のキャッシュは**直前の 1 件だけ**。一致条件は (セッション ID, 番号,
    要求フレームの CRC32)。同じ要求の再送には**操作を再実行せず**保存した応答を
    返す (RENAME / UNLINK / MKDIR / WRITE の二重実行を防ぐ)。HELLO で消える
  - 要求を受けてから SFS_FIRST_BYTE_S を過ぎた応答は**送らない** (ゲストの
    1 試行の期限を過ぎている = 次の試行の最中に古い番号が届くだけ)
  - BYE を受けたセッション、`sfs run` の行末 EOT の後には何も送らない
  - 未知のセッション ID の要求には SFS_T_ERR (OS32_ERR_STALE) で答える
    (ホストが再起動した)
  - パスは `/` 始まりのルート相対。`..` / `.` / `\\` / NUL の要素は断り、
    **realpath** で根の外 (symlink 経由も) を断る
"""
import errno
import os
import random
import stat as statmod
import struct
import time
import zlib

# ---- fs/sfs_proto.h の写し --------------------------------------------------
ENQ = 0x05
EOT = 0x04
MAGIC = b"SF"
VERSION = 1
HDR_LEN = 12
CRC_LEN = 4
MAX_PAYLOAD = 512
MAX_FRAME = HDR_LEN + MAX_PAYLOAD + CRC_LEN
READ_MAX = MAX_PAYLOAD - 4
PATH_MAX = 255

T_HELLO = 0x01
T_BYE = 0x02
T_STAT = 0x10
T_LIST = 0x11
T_READ = 0x12
T_WRITE = 0x13
T_MKDIR = 0x14
T_RMDIR = 0x15
T_UNLINK = 0x16
T_RENAME = 0x17
T_LOG = 0x20
T_EXIT = 0x21
T_RESP = 0x80
T_ERR = 0xFF

WF_TRUNC = 0x01
KIND_FILE = 1
KIND_DIR = 2
KIND_OTHER = 3
XF_NOT_QUIET = 0x01
XF_DEAD = 0x02

FIRST_BYTE_S = 2.0          # SFS_FIRST_BYTE_MS
GAP_S = 0.1                 # SFS_GAP_MS
# 振り分けで「フレームの途中で途切れた」と見なす長さ。ゲストの隔離 (500ms)
# より短く、バイト間 (100ms) より長く取る。
DEMUX_STALL_S = 0.3

# ---- OS32_ERR_* (sdk/include/os32/os32_kapi_shared.h の写し) ----------------
ERR_IO = -1
ERR_NOTFOUND = -2
ERR_NOSPC = -4
ERR_EXIST = -5
ERR_NOTDIR = -6
ERR_NOTEMPTY = -7
ERR_ISDIR = -8
ERR_INVAL = -9
ERR_STALE = -11
ERR_NAMETOOLONG = -16

ERRNO_MAP = {
    errno.ENOENT: ERR_NOTFOUND,
    errno.EEXIST: ERR_EXIST,
    errno.ENOTDIR: ERR_NOTDIR,
    errno.ENOTEMPTY: ERR_NOTEMPTY,
    errno.EISDIR: ERR_ISDIR,
    errno.ENOSPC: ERR_NOSPC,
    errno.ENAMETOOLONG: ERR_NAMETOOLONG,
    errno.EINVAL: ERR_INVAL,
}


class SfsError(Exception):
    def __init__(self, code):
        Exception.__init__(self, code)
        self.code = code


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def encode(ftype, sid, seq, payload=b""):
    payload = bytes(payload)
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    head = struct.pack("<BIHH", ftype, sid & 0xFFFFFFFF, seq & 0xFFFF,
                       len(payload))
    body = head + payload
    return bytes([ENQ]) + MAGIC + body + struct.pack("<I", crc32(body))


class Frame(object):
    __slots__ = ("type", "sid", "seq", "payload", "crc")

    def __init__(self, ftype, sid, seq, payload, crc):
        self.type = ftype
        self.sid = sid
        self.seq = seq
        self.payload = payload
        self.crc = crc

    def __repr__(self):
        return "Frame(type=%#x sid=%#x seq=%d len=%d)" % (
            self.type, self.sid, self.seq, len(self.payload))


def try_frame(buf, i):
    """buf[i] が ENQ のとき、そこからのフレームを判定する。

    戻り値:
      ('need', 0)          まだ足りない
      ('text', 1)          フレームではない (ENQ 1 バイトを文字として進める)
      ('bad', n)           印と長さは合うが CRC が違う (n バイトを捨てる)
      ('frame', n, Frame)  フレーム (n バイト)
    """
    avail = len(buf) - i
    if avail < 3:
        if avail >= 2 and buf[i + 1] != MAGIC[0]:
            return ("text", 1)
        return ("need", 0)
    if bytes(buf[i + 1:i + 3]) != MAGIC:
        return ("text", 1)
    if avail < HDR_LEN:
        return ("need", 0)
    ftype, sid, seq, ln = struct.unpack_from("<BIHH", bytes(buf[i + 3:i + HDR_LEN]))
    if ln > MAX_PAYLOAD:
        return ("text", 1)
    total = HDR_LEN + ln + CRC_LEN
    if avail < total:
        return ("need", 0)
    body = bytes(buf[i + 3:i + HDR_LEN + ln])
    want = struct.unpack_from("<I", bytes(buf[i + HDR_LEN + ln:i + total]))[0]
    if crc32(body) != want:
        return ("bad", total)
    return ("frame", total, Frame(ftype, sid, seq,
                                  bytes(buf[i + HDR_LEN:i + HDR_LEN + ln]),
                                  want))


class Demux(object):
    """線のバイトを「文字 / フレーム / EOT」に振り分ける (`sfs run` の行の中だけ)。

    - フレームの外の EOT は行の終わり。**フレームの中の 0x04 は本体**
    - 印 ('SF') と長さが合って CRC だけ違うものは**フレームごと捨てる**
      (中身の 0x04 を行の終わりと読まない)
    - フレームの途中で DEMUX_STALL_S 途切れたら、その ENQ は文字として流す
    """

    def __init__(self, stall_s=DEMUX_STALL_S):
        self.buf = bytearray()
        self.stall_s = stall_s
        self.last_rx = None
        self.bad_frames = 0

    def feed(self, data, now):
        if data:
            self.buf += data
            self.last_rx = now
        return self._run(now)

    def poll(self, now):
        """受信が無いあいだに呼ぶ (途切れたフレームを文字として流す)。"""
        return self._run(now)

    def _run(self, now):
        events = []
        text_start = 0
        i = 0
        buf = self.buf
        while i < len(buf):
            b = buf[i]
            if b == EOT:
                if i > text_start:
                    events.append(("text", bytes(buf[text_start:i])))
                events.append(("eot",))
                i += 1
                text_start = i
                continue
            if b != ENQ:
                i += 1
                continue
            r = try_frame(buf, i)
            if r[0] == "need":
                stalled = (self.last_rx is not None and
                           now - self.last_rx >= self.stall_s)
                if stalled:
                    i += 1           # ENQ を文字として進める
                    continue
                break
            if r[0] == "text":
                i += 1
                continue
            if i > text_start:
                events.append(("text", bytes(buf[text_start:i])))
            if r[0] == "bad":
                self.bad_frames += 1
            else:
                events.append(("frame", r[2]))
            i += r[1]
            text_start = i
        # i が末尾なら全部確定。途中 (待ちのフレーム) なら、その手前まで確定
        if i > text_start:
            events.append(("text", bytes(buf[text_start:i])))
            text_start = i
        del buf[:text_start]
        return events


def _err(e):
    if isinstance(e, SfsError):
        return e.code
    if isinstance(e, OSError):
        return ERRNO_MAP.get(e.errno, ERR_IO)
    return ERR_IO


class HostFS(object):
    """根 root の下だけを見せる。パスは bytes のまま扱う (名前を化けさせない)。"""

    def __init__(self, root):
        self.root = os.path.realpath(os.fsencode(root))
        if not os.path.isdir(self.root):
            raise ValueError("serve-host root is not a directory: %r" % root)

    def resolve(self, wire, allow_root=True):
        """ワイヤ上のパス (bytes、`/` 始まり) → ホストのパス。断るときは SfsError。"""
        if not wire.startswith(b"/"):
            raise SfsError(ERR_INVAL)
        comps = [c for c in wire[1:].split(b"/") if c != b""]
        for c in comps:
            if c in (b".", b"..") or b"\\" in c or b"\x00" in c:
                raise SfsError(ERR_INVAL)
            if len(c) > PATH_MAX:
                raise SfsError(ERR_NAMETOOLONG)
        if not comps and not allow_root:
            raise SfsError(ERR_INVAL)
        path = os.path.join(self.root, *comps) if comps else self.root
        real = os.path.realpath(path)
        if real != self.root and not real.startswith(self.root + b"/"):
            raise SfsError(ERR_INVAL)       # symlink で根の外へ出る
        return path

    # ---- 操作 (戻り値は応答ペイロードの status の後ろ、または例外) ----
    def stat(self, wire):
        st = os.stat(self.resolve(wire))
        if statmod.S_ISDIR(st.st_mode):
            kind, size = KIND_DIR, 0
        elif statmod.S_ISREG(st.st_mode):
            kind, size = KIND_FILE, st.st_size
        else:
            kind, size = KIND_OTHER, 0
        if size > 0xFFFFFFFF:
            raise SfsError(ERR_IO)
        mtime = int(st.st_mtime)
        if mtime < 0 or mtime > 0xFFFFFFFF:
            mtime = 0
        return 0, struct.pack("<BII", kind, size, mtime)

    def list(self, wire, cookie):
        path = self.resolve(wire)
        names = sorted(os.listdir(path))
        out = bytearray()
        room = MAX_PAYLOAD - 4 - 4
        idx = cookie
        while idx < len(names):
            name = names[idx]
            if len(name) > PATH_MAX or b"\\" in name:
                idx += 1                     # 見せられない名前は飛ばす
                continue
            full = os.path.join(path, name)
            try:
                st = os.stat(full)
                if statmod.S_ISDIR(st.st_mode):
                    kind, size = KIND_DIR, 0
                elif statmod.S_ISREG(st.st_mode):
                    kind, size = KIND_FILE, min(st.st_size, 0xFFFFFFFF)
                else:
                    kind, size = KIND_OTHER, 0
            except OSError:
                kind, size = KIND_OTHER, 0   # 壊れた symlink など
            ent = struct.pack("<BIB", kind, size, len(name)) + name
            if len(out) + len(ent) > room:
                break
            out += ent
            idx += 1
        nxt = idx if idx < len(names) else 0
        return 0, struct.pack("<I", nxt) + bytes(out)

    def read(self, wire, offset, count):
        count = min(count, READ_MAX)
        with open(self.resolve(wire), "rb") as f:
            f.seek(offset)
            data = f.read(count)
        return len(data), data

    def write(self, wire, offset, flags, data):
        path = self.resolve(wire, allow_root=False)
        if os.path.isdir(path):
            raise SfsError(ERR_ISDIR)
        if flags & WF_TRUNC:
            mode = "wb"
        else:
            mode = "r+b" if os.path.exists(path) else "w+b"
        with open(path, mode) as f:
            f.seek(offset)
            f.write(data)
        return len(data), b""

    def mkdir(self, wire):
        os.mkdir(self.resolve(wire, allow_root=False))
        return 0, b""

    def rmdir(self, wire):
        os.rmdir(self.resolve(wire, allow_root=False))
        return 0, b""

    def unlink(self, wire):
        path = self.resolve(wire, allow_root=False)
        if os.path.isdir(path) and not os.path.islink(path):
            raise SfsError(ERR_ISDIR)
        os.unlink(path)
        return 0, b""

    def rename(self, old, new):
        src = self.resolve(old, allow_root=False)
        dst = self.resolve(new, allow_root=False)
        os.replace(src, dst)
        return 0, b""


def _take_path(p, at):
    if at >= len(p):
        raise SfsError(ERR_INVAL)
    n = p[at]
    if at + 1 + n > len(p):
        raise SfsError(ERR_INVAL)
    return bytes(p[at + 1:at + 1 + n]), at + 1 + n


class Server(object):
    """要求フレーム → 応答フレーム。時計と乱数は差し替えられる (試験用)。"""

    def __init__(self, fs, now=time.monotonic, rng=None, log=None):
        self.fs = fs
        self.now = now
        self.rng = rng or random.SystemRandom()
        self.log = log or (lambda msg: None)
        self.sid = 0                 # 今のセッション (0 = 無い)
        self.closed = set()          # BYE を受けたセッション
        self.last_closed = 0
        self.cache_key = None        # (sid, seq, 要求の CRC32)
        self.cache_resp = None
        self.executed = 0            # 実際に操作した回数 (試験用)
        self.replayed = 0            # 保存した応答を返した回数
        self.late = 0                # 期限を過ぎて送らなかった応答
        self.stale = 0
        self.log_text = bytearray()
        self.exit = None             # (code, dropped, flags)

    # 要求 → (応答 bytes または None)
    def handle(self, fr):
        t = fr.type
        if t == T_HELLO:
            return self._hello(fr)
        if t == T_BYE:
            if fr.sid and fr.sid == self.sid:
                self.closed.add(fr.sid)
                self.last_closed = fr.sid
                self.sid = 0
                self.cache_key = None
                self.cache_resp = None
            return None
        if t in (T_LOG, T_EXIT):
            if fr.sid and fr.sid in (self.sid, self.last_closed):
                if t == T_LOG:
                    self.log_text += fr.payload
                elif len(fr.payload) >= 12:
                    self.exit = struct.unpack_from("<iII", fr.payload)
            return None
        if t & T_RESP:
            return None                  # 応答の向きのもの (こちらには来ない)
        if fr.sid in self.closed:
            return None                  # BYE の後は何も送らない
        if fr.sid == 0 or fr.sid != self.sid:
            self.stale += 1
            return encode(T_ERR, fr.sid, fr.seq, struct.pack("<i", ERR_STALE))
        key = (fr.sid, fr.seq, fr.crc)
        if key == self.cache_key:
            self.replayed += 1
            return self.cache_resp
        resp = self._execute(fr)
        self.cache_key = key
        self.cache_resp = resp
        return resp

    def _hello(self, fr):
        key = (0, fr.seq, fr.crc)
        if key == self.cache_key and self.sid:
            self.replayed += 1
            return self.cache_resp
        if len(fr.payload) < 8 or fr.sid != 0 or fr.seq != 0:
            return None
        ver, maxp, nonce = struct.unpack_from("<HHI", fr.payload)
        if self.sid:
            # 前のセッションが BYE 無しで終わった (ゲストの再起動など)。
            # そちらへは以後何も送らない
            self.closed.add(self.sid)
        sid = 0
        while sid == 0 or sid in self.closed:
            sid = self.rng.getrandbits(32)
        self.sid = sid
        resp = encode(T_HELLO | T_RESP, sid, 0,
                      struct.pack("<iHHI", 0, VERSION, MAX_PAYLOAD, nonce))
        self.cache_key = key
        self.cache_resp = resp
        self.log("[serve-host] HELLO v%d max=%d -> session %08x" % (ver, maxp, sid))
        return resp

    def _execute(self, fr):
        p = fr.payload
        t = fr.type
        body = b""
        self.executed += 1
        try:
            if t == T_STAT:
                path, _ = _take_path(p, 0)
                st, body = self.fs.stat(path)
            elif t == T_LIST:
                if len(p) < 4:
                    raise SfsError(ERR_INVAL)
                cookie = struct.unpack_from("<I", p)[0]
                path, _ = _take_path(p, 4)
                st, body = self.fs.list(path, cookie)
            elif t == T_READ:
                if len(p) < 6:
                    raise SfsError(ERR_INVAL)
                off, cnt = struct.unpack_from("<IH", p)
                path, _ = _take_path(p, 6)
                st, body = self.fs.read(path, off, cnt)
            elif t == T_WRITE:
                if len(p) < 5:
                    raise SfsError(ERR_INVAL)
                off, flags = struct.unpack_from("<IB", p)
                path, at = _take_path(p, 5)
                st, body = self.fs.write(path, off, flags, p[at:])
            elif t in (T_MKDIR, T_RMDIR, T_UNLINK):
                path, _ = _take_path(p, 0)
                op = {T_MKDIR: self.fs.mkdir, T_RMDIR: self.fs.rmdir,
                      T_UNLINK: self.fs.unlink}[t]
                st, body = op(path)
            elif t == T_RENAME:
                old, at = _take_path(p, 0)
                new, _ = _take_path(p, at)
                st, body = self.fs.rename(old, new)
            else:
                raise SfsError(ERR_INVAL)
        except Exception as e:          # noqa: BLE001 — 何でも status にする
            st, body = _err(e), b""
        return encode(t | T_RESP, fr.sid, fr.seq, struct.pack("<i", st) + body)


def sfs_child(line):
    """`sfs run <コマンド行>` の子を取り出す (userland/shell/serial_watchdog.c の
    rsh_sfs_child と同じ規則)。形が違えば None。"""
    s = line.lstrip(" \t")
    if not s.startswith("sfs") or s[3:4] not in (" ", "\t"):
        return None
    s = s[3:].lstrip(" \t")
    if not s.startswith("run") or s[3:4] not in (" ", "\t"):
        return None
    s = s[3:].lstrip(" \t")
    return s or None


def serve_line(port, line, server, timeout_s, out, now=time.monotonic,
               sleep=time.sleep):
    """`sfs run ...` の 1 行を送り、行末の EOT まで線を振り分けて答える。

    port は read(n) / write(b) / flush() / reset_input_buffer() を持つもの。
    **時間切れは「進捗が無い時間」** — 何か受け取るたびに期限を延ばす
    (hsync boot は何分もかかりうる)。**配信中は reset_input_buffer を使わない**。
    戻り値 dict: text (rshell の文字), eot (見たか), exit (code, dropped, flags)
    または None, sent / late / bad_frames。
    """
    dm = Demux()
    text = bytearray()
    port.reset_input_buffer()            # 送る前だけ (配信中は使わない)
    port.write(line.encode("utf-8") + b"\n")
    port.flush()
    deadline = now() + timeout_s
    eot = False
    sent = 0
    while not eot:
        t = now()
        if t >= deadline:
            break
        chunk = port.read(MAX_FRAME)
        t = now()
        if chunk:
            deadline = t + timeout_s
            events = dm.feed(chunk, t)
        else:
            events = dm.poll(t)
        for ev in events:
            if ev[0] == "text":
                text += ev[1]
                out.write(ev[1].decode("utf-8", errors="replace"))
            elif ev[0] == "frame":
                had_log = len(server.log_text)
                resp = server.handle(ev[1])
                if len(server.log_text) != had_log:
                    out.write(bytes(server.log_text[had_log:]).decode(
                        "utf-8", errors="replace"))
                if resp is not None:
                    # **期限を過ぎた応答は送らない** — ゲストは次の試行に
                    # 入っていて、古い番号は捨てられるだけ (線を無駄に塞ぐ)
                    if now() - t > FIRST_BYTE_S:
                        server.late += 1
                        continue
                    port.write(resp)
                    port.flush()
                    sent += 1
            elif ev[0] == "eot":
                eot = True
                break
        if not chunk and not eot:
            sleep(0.001)
    # 行が終わった = セッションも終わっている。以後このセッションには答えない
    if server.sid:
        server.closed.add(server.sid)
        server.last_closed = server.sid
        server.sid = 0
    return {"text": bytes(text), "eot": eot, "exit": server.exit,
            "sent": sent, "late": server.late, "bad_frames": dm.bad_frames}
