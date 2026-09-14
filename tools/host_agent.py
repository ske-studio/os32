#!/usr/bin/env python3
"""OS32 Host Agent v2 — ワイヤ v2 (docs/tasks/network/TASK_N0.md §1b / §2d)。

OS32 リンクプロトコル (raw Ethernet, EtherType 0x88B5) を話すホスト側常駐。
v1 (16B ヘッダ・Stop-and-Wait・セッション無し) を置き換える。

  20B ヘッダ (LE 明示直列化): op u8 @0, flags u8 @1, epoch u16 @2, seq u32 @4,
                              ack u32 @8, length u16 @12, rid u32 @14, sess u16 @18
  3 way HELLO (SYN / SYN-ACK / CONFIRM / ESTABLISHED)、sess は **Agent が採番**
  (永続カウンタ sess.txt、再使用せず 65535 で枯渇停止)
  rid 台帳 (ACTIVE ≤ 2 / RELEASED 墓標 / HOLE、high_water、規則 (1)〜(5))
  RESPONSE flags bit0 = 制御結果 (PROCESSING / TOMBSTONE / NO_SLOT)、
  flags 0 = 業務結果 (HTTP ステータスをそのまま載せる)
  RELEASE → flags bit0 の ACK、WINDOW を受けた rid だけ DATA を流す

トランスポートは 2 つ。どちらも QEMU socket と同じ枠 (4B BE 長 + フレーム):
  - TCP   : ai-debug のソケットバックエンド (NP2NETSOCK=127.0.0.1:8026)
  - UNIX  : --unix <path> (ホスト TDD が実 Agent を子プロセスで結ぶ)

  python3 tools/host_agent.py                            # listen 127.0.0.1:8026
  python3 tools/host_agent.py --connect 127.0.0.1:8026   # NP2NETSOCK=listen:8026
  python3 tools/host_agent.py --unix /tmp/os32.sock --state-dir /tmp/st --quiet
"""
import argparse
import os
import random
import socket
import struct
import sys
import time
import urllib.request

# ---------------------------------------------------------------- ワイヤ定数
LINK_ETHERTYPE = 0x88B5
ETH_HDR = 14
LINK_HDR = 20
LINK_MAX_PAYLOAD = 1400
LINK_MIN_FRAME = 60

OP_HELLO = 1
OP_REQUEST = 2
OP_RESPONSE = 3
OP_DATA = 4
OP_EOF = 5
OP_ACK = 6
OP_WINDOW = 7
OP_WDATA = 8
OP_STATUS = 9
OP_RELEASE = 10
OPNAME = {1: "HELLO", 2: "REQUEST", 3: "RESPONSE", 4: "DATA", 5: "EOF",
          6: "ACK", 7: "WINDOW", 8: "WDATA", 9: "STATUS", 10: "RELEASE"}

# HELLO の段階 (flags)
HS_SYN = 0
HS_SYNACK = 1
HS_CONFIRM = 2
HS_ESTAB = 3

# flags bit0
F_CTRL = 0x01      # RESPONSE: 制御結果 (業務結果ではない)
F_RELACK = 0x01    # ACK: RELEASE への ACK

# 制御 RESPONSE の status (リンク符号。業務の HTTP ステータスとは別空間)
CTL_PROCESSING = 1
CTL_TOMBSTONE = 2
CTL_NO_SLOT = 3

SESS_MAX = 0xFFFF
RID_MAX = 0xFFFFFFFF
EPOCH_MAX = 0xFFFF
ACTIVE_MAX = 2          # 同時受付 (OS32 のハンドル数と同じ)
TOMB_WINDOW = 8         # 墓標を落としてよい高さ (high_water - 8)
HOLE_FILL_MAX = 64      # 一度に作る HOLE の上限 (異常な rid 跳躍への歯止め)
FRAME_PAGES = 6         # 最大フレームが占める SRAM ページ数 (保守的)
STREAM_PLEN = 512       # 本文ストリームの 1 フレームあたりのバイト数

# op ごとの payload 長 (min, max)。一致しないフレームは捨てる (v2 パーサの検査)
PAYLEN = {
    OP_REQUEST: (1, LINK_MAX_PAYLOAD),
    OP_WDATA: (1, LINK_MAX_PAYLOAD),
    OP_RESPONSE: (6, 6),
    OP_ACK: (0, 0),
    OP_STATUS: (0, 0),
    OP_RELEASE: (0, 0),
    OP_DATA: (1, LINK_MAX_PAYLOAD),
    OP_EOF: (0, 0),
    OP_WINDOW: (2, 2),
}
# HELLO は段階ごとに長さが違う
HELLO_PAYLEN = {HS_SYN: 0, HS_SYNACK: 6, HS_CONFIRM: 0, HS_ESTAB: 2}


def hdr_pack(op, flags, epoch, seq, ack, length, rid, sess):
    """20B リンクヘッダを LE で明示的に直列化する ("<" で詰め物を作らない)。"""
    return struct.pack("<BBHIIHIH", op & 0xFF, flags & 0xFF, epoch & 0xFFFF,
                       seq & 0xFFFFFFFF, ack & 0xFFFFFFFF, length & 0xFFFF,
                       rid & 0xFFFFFFFF, sess & 0xFFFF)


def hdr_unpack(buf):
    return struct.unpack("<BBHIIHIH", buf[:LINK_HDR])


def mac_str(b):
    return ":".join("%02x" % x for x in b)


def parse_mac(s):
    return bytes(int(x, 16) for x in s.split(":"))


# ------------------------------------------------------------ トランスポート
class FrameStream:
    """4B BE 長 + raw Ethernet フレーム (QEMU socket と同じ枠)。"""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def send(self, frame):
        self.sock.sendall(struct.pack(">I", len(frame)) + frame)

    def recv_frames(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("peer closed")
        self.buf += data
        out = []
        while len(self.buf) >= 4:
            n = struct.unpack(">I", self.buf[:4])[0]
            if n == 0 or n > 65536:
                raise ConnectionError("bad frame length %d" % n)
            if len(self.buf) < 4 + n:
                break
            out.append(self.buf[4:4 + n])
            self.buf = self.buf[4 + n:]
        return out


class ListSink:
    """試験用: 送ったフレームを溜めるだけの出口。"""

    def __init__(self):
        self.frames = []

    def send(self, frame):
        self.frames.append(frame)


class Pcap:
    def __init__(self, path):
        self.f = open(path, "wb")
        self.f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))

    def write(self, frame):
        t = time.time()
        self.f.write(struct.pack("<IIII", int(t), int((t % 1) * 1e6), len(frame), len(frame)))
        self.f.write(frame)
        self.f.flush()


# ------------------------------------------------------------------ sess 採番
class SessCounter:
    """セッション ID の**永続**採番 (TASK_N0 §1b、往復 4 の R3)。

    Agent 再起動をまたいでも再使用しない。65535 に達したら新セッションの SYN に
    応答せず、ログに枯渇を出して止まる (運用者が全 OS32 を止めて sess.txt を
    消して再開する)。state_dir が None のときはメモリ内だけ (試験の既定)。
    """

    def __init__(self, state_dir=None, start=0):
        self.path = os.path.join(state_dir, "sess.txt") if state_dir else None
        self.value = start
        if self.path and os.path.exists(self.path):
            try:
                with open(self.path) as f:
                    self.value = int(f.read().strip() or "0")
            except (OSError, ValueError):
                self.value = start

    def exhausted(self):
        return self.value >= SESS_MAX

    def alloc(self):
        if self.exhausted():
            return None
        self.value += 1
        if self.path:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                f.write("%d\n" % self.value)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self.path)
        return self.value


# -------------------------------------------------------------------- Agent
class HostAgent:
    def __init__(self, mac, state_dir=None, agent_gen=None, quiet=False,
                 rng=None, sess_start=0, allow_net=True, file_root=None):
        self.mac = mac
        self.quiet = quiet
        self.rng = rng or random.Random()
        self.agent_gen = agent_gen if agent_gen is not None else self.rng.randint(1, 0xFFFF)
        self.sessions = SessCounter(state_dir, sess_start)
        self.allow_net = allow_net
        self.file_root = file_root
        # ---- 現行セッション (CONFIRM で切り替わる) ----
        self.sess = 0
        self.epoch = 0
        self.peer = None            # OS32 の MAC
        self.established = False
        # ---- 候補 (SYN-ACK で 1 件だけ保持。新しい SYN で上書き) ----
        self.cand = None
        # ---- rid 台帳 ----
        self.ledger = {}            # rid -> dict(st=ACTIVE/RELEASED/HOLE, ...)
        self.high_water = 0
        # ---- 観測 ----
        self.stopped = False        # sess 枯渇で停止した
        self.counts = {"hello": 0, "request": 0, "wdata": 0, "ack": 0,
                       "window": 0, "status": 0, "release": 0, "dropped": 0,
                       "no_slot": 0, "tombstone": 0, "processing": 0}

    # ------------------------------------------------------------ ログ
    def log(self, *a):
        if not self.quiet:
            print(*a, flush=True)

    # ------------------------------------------------------ フレーム組み立て
    def build(self, dst, op, flags=0, seq=0, ack=0, rid=0, payload=b"",
              sess=None, epoch=None):
        hdr = hdr_pack(op, flags,
                       self.epoch if epoch is None else epoch,
                       seq, ack, len(payload), rid,
                       self.sess if sess is None else sess)
        frame = dst + self.mac + struct.pack(">H", LINK_ETHERTYPE) + hdr + payload
        if len(frame) < LINK_MIN_FRAME:
            frame += b"\x00" * (LINK_MIN_FRAME - len(frame))
        return frame

    # ------------------------------------------------------------- 入口
    def handle(self, frame, out, pcap=None):
        if pcap:
            pcap.write(frame)
        if len(frame) < ETH_HDR + LINK_HDR:
            self.counts["dropped"] += 1
            return
        dst, src = frame[:6], frame[6:12]
        et = struct.unpack(">H", frame[12:14])[0]
        if et != LINK_ETHERTYPE:
            self.counts["dropped"] += 1
            return
        op, flags, epoch, seq, ack, plen, rid, sess = hdr_unpack(frame[ETH_HDR:])
        payload = frame[ETH_HDR + LINK_HDR:ETH_HDR + LINK_HDR + plen]
        if len(payload) != plen:
            self.counts["dropped"] += 1        # 宣言長がフレームに収まらない
            return
        if not self._paylen_ok(op, flags, plen):
            self.counts["dropped"] += 1
            return

        if op == OP_HELLO:
            self._on_hello(src, flags, epoch, seq, ack, sess, payload, out)
            return
        # HELLO 以外は sess / epoch が現行と一致するものだけ
        if not self.established or sess != self.sess or epoch != self.epoch:
            self.counts["dropped"] += 1
            return
        if src != self.peer:
            self.counts["dropped"] += 1
            return

        if op == OP_REQUEST:
            self.counts["request"] += 1
            self._on_request(rid, seq, payload, out)
        elif op == OP_WDATA:
            self.counts["wdata"] += 1
            self._on_wdata(rid, seq, payload, out)
        elif op == OP_STATUS:
            self.counts["status"] += 1
            self._on_status(rid, out)
        elif op == OP_RELEASE:
            self.counts["release"] += 1
            self._on_release(rid, out)
        elif op == OP_WINDOW:
            self.counts["window"] += 1
            self._on_window(rid, ack, struct.unpack("<H", payload[:2])[0], out)
        elif op == OP_ACK:
            self.counts["ack"] += 1
            self.note_rid(rid)
        else:
            self.counts["dropped"] += 1

    def _paylen_ok(self, op, flags, plen):
        if op == OP_HELLO:
            want = HELLO_PAYLEN.get(flags)
            return want is not None and plen == want
        lim = PAYLEN.get(op)
        if lim is None:
            return False
        return lim[0] <= plen <= lim[1]

    # ------------------------------------------------------- 3 way HELLO
    def _on_hello(self, src, flags, epoch, seq, ack, sess, payload, out):
        self.counts["hello"] += 1
        if flags == HS_SYN:
            self._on_syn(src, epoch, seq, sess, out)
        elif flags == HS_CONFIRM:
            self._on_confirm(src, epoch, seq, ack, sess, out)
        # SYN-ACK / ESTABLISHED は Agent 発なので受けたら無視

    def _on_syn(self, src, epoch, nonce, req_sess, out):
        """候補を 1 件だけ作って SYN-ACK を返す。**現行セッションは変えない**。"""
        # **SYN のたびに候補を作り直す** (nonce は HELLO ごとに +1 されるので、
        # 同じ nonce の SYN は「同じ RTC 秒に再起動した別の OS32」でもありうる
        # = 往復 3 の B3。候補を使い回すと再起動が旧 sess を貰ってしまう)。
        # 採番するのは新セッションの SYN だけで、セッション内の再同期 (req_sess
        # が現行 sess) は同じ sess のまま — 再送で sess を減らさない。
        if req_sess != 0 and self.established and req_sess == self.sess:
            assigned = self.sess                      # セッション内の再同期
        else:
            assigned = self.sessions.alloc()
            if assigned is None:
                self.stopped = True
                self.log("sess exhausted (%d): refusing new sessions. "
                         "stop every OS32, remove sess.txt, restart." % SESS_MAX)
                return
        c = {"mac": src, "sess": assigned, "epoch": epoch,
             "os32_nonce": nonce, "agent_nonce": self.rng.randint(1, 0xFFFFFFFF),
             "req_sess": req_sess, "req_epoch": epoch}
        self.cand = c
        pl = struct.pack("<HHH", self.agent_gen, c["req_sess"], c["req_epoch"])
        out.send(self.build(src, OP_HELLO, HS_SYNACK, seq=c["os32_nonce"],
                            ack=c["agent_nonce"], payload=pl,
                            sess=c["sess"], epoch=c["epoch"]))
        self.log("SYN from %s (sess %d epoch %d) -> SYN-ACK sess %d"
                 % (mac_str(src), req_sess, epoch, c["sess"]))

    def _on_confirm(self, src, epoch, nonce, ack, sess, out):
        c = self.cand
        if c is None or c["mac"] != src or c["os32_nonce"] != nonce or \
                c["agent_nonce"] != ack or c["sess"] != sess or c["epoch"] != epoch:
            return                                    # 遅延した旧 CONFIRM
        if not (self.established and self.sess == c["sess"] and self.epoch == c["epoch"]):
            self._switch_to(c)
        out.send(self.build(src, OP_HELLO, HS_ESTAB, seq=c["agent_nonce"],
                            ack=c["os32_nonce"],
                            payload=struct.pack("<H", self.agent_gen)))
        self.log("CONFIRM ok -> ESTABLISHED sess %d epoch %d" % (self.sess, self.epoch))

    def _switch_to(self, c):
        new_sess = c["sess"] != self.sess
        self.sess, self.epoch, self.peer = c["sess"], c["epoch"], c["mac"]
        self.established = True
        if new_sess:
            self.ledger = {}                          # セッションが変われば墓標も捨てる
            self.high_water = 0
        else:
            # 同じ sess の epoch 更新: ACTIVE は捨て、HOLE は墓標へ、RELEASED は保つ
            for rid in list(self.ledger):
                st = self.ledger[rid]["st"]
                if st == "ACTIVE":
                    del self.ledger[rid]
                elif st == "HOLE":
                    self.ledger[rid] = {"st": "RELEASED"}

    # --------------------------------------------------------- rid 台帳
    def note_rid(self, rid):
        """どのフレームで見た rid でも high_water を上げ、隙間を HOLE にする。"""
        if rid == 0 or rid <= self.high_water:
            return
        first = self.high_water + 1
        if rid - first > HOLE_FILL_MAX:
            first = rid - HOLE_FILL_MAX               # 異常な跳躍への歯止め
        for r in range(first, rid):
            self.ledger.setdefault(r, {"st": "HOLE"})
        self.high_water = rid

    def _active_count(self):
        return sum(1 for e in self.ledger.values() if e["st"] == "ACTIVE")

    def _reap_tombstones(self):
        """規則 (5): RELEASED は rid <= high_water - 8 で落としてよい。
        ACTIVE / HOLE は落とさない (長寿命の結果と未受理の穴を守る)。"""
        cut = self.high_water - TOMB_WINDOW
        for rid in [r for r, e in self.ledger.items()
                    if e["st"] == "RELEASED" and r <= cut]:
            del self.ledger[rid]

    def _ctl(self, rid, status, out):
        out.send(self.build(self.peer, OP_RESPONSE, F_CTRL, rid=rid,
                            payload=struct.pack("<HI", status, 0)))

    def _biz(self, rid, status, length, out):
        out.send(self.build(self.peer, OP_RESPONSE, 0, rid=rid,
                            payload=struct.pack("<HI", status, length)))

    def _ack(self, rid, seq, out, flags=0):
        out.send(self.build(self.peer, OP_ACK, flags, rid=rid, ack=seq))

    # -------------------------------------------------------- REQUEST
    def _on_request(self, rid, seq, payload, out):
        if rid == 0 or seq != 0:
            self.counts["dropped"] += 1
            return
        known = self.ledger.get(rid)
        new = rid > self.high_water
        self.note_rid(rid)
        if known is not None and known["st"] == "ACTIVE":
            self._ack(rid, known["last_seq"], out)     # (2) 重複: 再実行しない
            if known.get("resp") is not None:
                self._biz(rid, known["resp"][0], known["resp"][1], out)
            return
        if known is not None and known["st"] == "RELEASED":
            self.counts["tombstone"] += 1              # (3) 墓標
            self._ctl(rid, CTL_TOMBSTONE, out)
            return
        if known is None and not new:
            self.counts["tombstone"] += 1              # (3) 落とした墓標
            self._ctl(rid, CTL_TOMBSTONE, out)
            return
        # (1) 新規 / (2') HOLE の受理
        if self._active_count() >= ACTIVE_MAX:
            self.counts["no_slot"] += 1
            self.ledger[rid] = {"st": "HOLE"}          # 穴として残す (墓標にしない)
            self._ctl(rid, CTL_NO_SLOT, out)
            return
        ent = {"st": "ACTIVE", "req": payload, "last_seq": 0, "decl": 0,
               "got": b"", "resp": None, "body": None, "gen": None,
               "deliver": None}
        self.ledger[rid] = ent
        self._ack(rid, 0, out)
        self._reap_tombstones()
        self._serve(rid, ent, out)

    # ---------------------------------------------------------- WDATA
    def _on_wdata(self, rid, seq, payload, out):
        ent = self.ledger.get(rid)
        self.note_rid(rid)
        if ent is None or ent["st"] != "ACTIVE":
            if ent is not None and ent["st"] == "RELEASED":
                self._ctl(rid, CTL_TOMBSTONE, out)
            return
        if ent["decl"] == 0 or ent["resp"] is not None:
            return                                     # 宣言長の無い要求 / 完了済み
        if seq == ent["last_seq"] + 1:
            room = ent["decl"] - len(ent["got"])
            ent["got"] += payload[:room]
            ent["last_seq"] = seq
        # 重複 / 先行は捨てて累積 ACK を返す (Go-Back-N)
        self._ack(rid, ent["last_seq"], out)
        if len(ent["got"]) >= ent["decl"]:
            self._finish_body(rid, ent, out)

    # --------------------------------------------------------- STATUS
    def _on_status(self, rid, out):
        ent = self.ledger.get(rid)
        self.note_rid(rid)
        if ent is not None and ent["st"] == "ACTIVE":
            if ent["resp"] is not None:
                self._biz(rid, ent["resp"][0], ent["resp"][1], out)   # 再提示
            else:
                self.counts["processing"] += 1
                self._ctl(rid, CTL_PROCESSING, out)
            return
        self.counts["tombstone"] += 1
        self._ctl(rid, CTL_TOMBSTONE, out)             # RELEASED / HOLE / 未知

    # -------------------------------------------------------- RELEASE
    def _on_release(self, rid, out):
        self.note_rid(rid)
        self.ledger[rid] = {"st": "RELEASED"}          # (4) 先着でも墓標を作る
        self._reap_tombstones()
        self._ack(rid, 0, out, flags=F_RELACK)

    # --------------------------------------------------------- WINDOW
    def _on_window(self, rid, ack, credit, out):
        ent = self.ledger.get(rid)
        self.note_rid(rid)
        if ent is None or ent["st"] != "ACTIVE" or ent["deliver"] is None:
            return
        d = ent["deliver"]
        if ack > d["acked"]:
            d["acked"] = ack
            d["stall"] = 0
        else:
            d["stall"] += 1
            if d["stall"] >= 8 and d["sent"] > d["acked"]:
                d["retx"] += d["sent"] - d["acked"]    # ack が進まない = 欠落
                d["sent"] = d["acked"]
                d["stall"] = 0
        d["credit"] = credit
        self._pump(rid, ent, out)

    def _pump(self, rid, ent, out):
        d = ent["deliver"]
        while d["sent"] < d["nframes"]:
            inflight = FRAME_PAGES * (d["sent"] - d["acked"])
            if inflight + FRAME_PAGES > d["credit"]:
                break
            seq = d["sent"] + 1
            if seq == d["dropseq"] and not d["dropped"]:
                d["dropped"] = True                    # 1 回だけ落とす (Go-Back-N)
                d["sent"] = seq
                continue
            off = (seq - 1) * d["plen"]
            ln = min(d["plen"], d["total"] - off)
            body = ent["gen"](off, ln) if ent["gen"] else ent["body"][off:off + ln]
            out.send(self.build(self.peer, OP_DATA, seq=seq, rid=rid, payload=body))
            d["sent"] = seq
            d["max_inflight"] = max(d["max_inflight"],
                                    FRAME_PAGES * (d["sent"] - d["acked"]))
        if d["sent"] >= d["nframes"] and not d["eof"]:
            out.send(self.build(self.peer, OP_EOF, seq=d["nframes"] + 1, rid=rid))
            d["eof"] = True

    # -------------------------------------------------------- サービス
    def _serve(self, rid, ent, out):
        """要求行を読む。宣言長のある要求は本文を待ち、無い要求はすぐ答える。"""
        line = ent["req"].decode("latin1").strip()
        parts = line.split()
        verb = parts[0].upper() if parts else ""
        try:
            if verb == "ECHO":
                ent["decl"] = int(parts[1])
                return                                  # WDATA を待つ
            if verb == "CLIP" and len(parts) >= 3 and parts[1].upper() == "PUT":
                ent["decl"] = int(parts[2])
                return
            if verb == "PRINT" and len(parts) >= 4 and parts[1].upper() == "DATA":
                ent["decl"] = int(parts[3])
                return
            if verb == "PUT":
                ent["decl"] = int(parts[-1])
                return
        except (IndexError, ValueError):
            self._answer(rid, ent, 400, b"", None, out)
            return
        self._service_now(rid, ent, verb, parts, line, out)

    def _service_now(self, rid, ent, verb, parts, line, out):
        if verb == "PING":
            self._answer(rid, ent, 200, b"", None, out)
        elif verb == "TIME":
            body = time.strftime("%Y-%m-%d %H:%M:%S").encode()
            self._answer(rid, ent, 200, body, None, out)
        elif verb == "GET":
            self._service_get(rid, ent, parts[1] if len(parts) > 1 else "", out)
        elif verb == "BULK":
            count, plen = int(parts[1]), int(parts[2])
            self._answer(rid, ent, 200, None,
                         (lambda off, ln: bytes((off + k) & 0xFF for k in range(ln))),
                         out, total=count * plen, plen=plen)
        elif verb == "STREAM":
            total, plen, drop = int(parts[1]), int(parts[2]), int(parts[3])
            self._answer(rid, ent, 200, None,
                         (lambda off, ln: bytes((off + k) & 0xFF for k in range(ln))),
                         out, total=total, plen=plen, dropseq=drop)
        else:
            self.log("unknown request: %r" % line)
            self._answer(rid, ent, 400, b"", None, out)

    def _service_get(self, rid, ent, resource, out):
        if resource.startswith("/pattern/"):
            try:
                n = int(resource[len("/pattern/"):])
            except ValueError:
                n = 0
            self._answer(rid, ent, 200, None,
                         (lambda off, ln: bytes((off + k) & 0xFF for k in range(ln))),
                         out, total=n)
        elif resource.startswith("http://") or resource.startswith("https://"):
            status, body = 502, b""
            if self.allow_net:
                try:
                    with urllib.request.urlopen(resource, timeout=10) as r:
                        body = r.read()
                        status = getattr(r, "status", 200) or 200
                except Exception as e:                 # noqa: BLE001 (何であれ 502)
                    self.log("GET %s failed: %s" % (resource, e))
            self._answer(rid, ent, status, body, None, out)
        elif resource.startswith("/file/"):
            path = resource[len("/file/"):]
            if self.file_root:
                path = os.path.join(self.file_root, path.lstrip("/"))
            try:
                with open(path, "rb") as fh:
                    self._answer(rid, ent, 200, fh.read(), None, out)
            except OSError:
                self._answer(rid, ent, 404, b"", None, out)
        elif resource == "/status/503":
            self._answer(rid, ent, 503, b"", None, out)     # B7: 業務の 503
        elif resource == "/status/410":
            self._answer(rid, ent, 410, b"gone body", None, out)  # B7: 本文付き 410
        else:
            self._answer(rid, ent, 404, b"", None, out)

    def _finish_body(self, rid, ent, out):
        """宣言長ぶんの WDATA を受け切った要求に答える。"""
        line = ent["req"].decode("latin1").strip()
        verb = line.split()[0].upper()
        if verb == "ECHO":
            self._answer(rid, ent, 200, ent["got"], None, out)   # 折り返し
        else:
            self._answer(rid, ent, 200, b"", None, out)

    def _answer(self, rid, ent, status, body, gen, out,
                total=None, plen=STREAM_PLEN, dropseq=0):
        if total is None:
            total = len(body) if body is not None else 0
        ent["body"] = body
        ent["gen"] = gen
        ent["resp"] = (status, total)
        if status == 200 and total > 0:
            nframes = (total + plen - 1) // plen
            ent["deliver"] = {"total": total, "plen": plen, "nframes": nframes,
                              "sent": 0, "acked": 0, "credit": 0, "eof": False,
                              "dropseq": dropseq, "dropped": False, "stall": 0,
                              "retx": 0, "max_inflight": 0}
        self._biz(rid, status, total, out)
        self.log("rid %d -> %d, %d bytes" % (rid, status, total))


# --------------------------------------------------------------- 常駐ループ
def open_stream(args):
    if args.unix:
        if os.path.exists(args.unix):
            os.unlink(args.unix)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(args.unix)
        srv.listen(1)
        print("listening on unix:%s" % args.unix, flush=True)
        conn, _ = srv.accept()
        srv.close()
        return FrameStream(conn)
    if args.connect:
        host, port = args.connect.rsplit(":", 1)
        s = socket.create_connection((host, int(port)))
        print("connected to %s" % args.connect, flush=True)
        return FrameStream(s)
    host, port = args.listen.rsplit(":", 1)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, int(port)))
    srv.listen(1)
    print("listening on %s" % args.listen, flush=True)
    conn, peer = srv.accept()
    print("peer attached from %s:%d" % peer, flush=True)
    return FrameStream(conn)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1:8026", metavar="HOST:PORT")
    ap.add_argument("--connect", metavar="HOST:PORT")
    ap.add_argument("--unix", metavar="PATH", help="UNIX ソケットで待ち受ける (ホスト TDD 用)")
    ap.add_argument("--state-dir", metavar="DIR", help="sess.txt (永続採番) を置く場所")
    ap.add_argument("--agent-gen", type=int, metavar="N", help="Agent 世代を固定する (試験用)")
    ap.add_argument("--mac", default="02:00:5e:00:00:01")
    ap.add_argument("--pcap", metavar="FILE")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--offline", action="store_true", help="実 HTTP を出さない (常に 502)")
    ap.add_argument("--file-root", metavar="DIR", help="GET /file/ の許可ルート")
    ap.add_argument("--once", action="store_true", help="exit after the peer disconnects")
    args = ap.parse_args()

    if args.state_dir:
        os.makedirs(args.state_dir, exist_ok=True)
    agent = HostAgent(parse_mac(args.mac), state_dir=args.state_dir,
                      agent_gen=args.agent_gen, quiet=args.quiet,
                      allow_net=not args.offline, file_root=args.file_root)
    pcap = Pcap(args.pcap) if args.pcap else None
    print("host_agent v2 mac %s agent-gen %d (wire v2, 3-way HELLO, rid ledger)"
          % (args.mac, agent.agent_gen), flush=True)

    while True:
        try:
            stream = open_stream(args)
        except OSError as e:
            print("connect failed: %s" % e, flush=True)
            if args.connect:
                time.sleep(1.0)
                continue
            return 1
        try:
            while True:
                for frame in stream.recv_frames():
                    agent.handle(frame, stream, pcap)
        except ConnectionError as e:
            print("disconnected: %s (%s)" % (e, agent.counts), flush=True)
            if args.once:
                return 0
            if args.connect or args.unix:
                if args.unix:
                    continue
                time.sleep(1.0)


if __name__ == "__main__":
    sys.exit(main())
