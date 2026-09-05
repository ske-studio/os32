#!/usr/bin/env python3
"""OS32 Host Agent (L0: Stop-and-Wait) — docs/tasks/network/LINK_PLAN.md §6.

OS32 リンクプロトコルを話す最小のホスト側エージェント。np2net_helper.py の
スタブ (ARP/ICMP/UDP) を置き換えるもので、この段階 (L0) では:
  - HELLO を受けたら自分の MAC を載せた HELLO を返す
  - REQUEST を受けたら RESPONSE を返す (ack=req.seq、本文は "PONG "+payload)
  - ACK は受けるだけ
ai-debug のソケットバックエンド (NP2NETSOCK=127.0.0.1:8026) に接続 / 待受する。
ワイヤ形式は QEMU socket と同じ (4B BE 長 + raw Ethernet フレーム)。

  python3 tools/host_agent.py                       # listen 127.0.0.1:8026 (NP2NETSOCK=connect)
  python3 tools/host_agent.py --connect 127.0.0.1:8026   # NP2NETSOCK=listen:8026
  python3 tools/host_agent.py --pcap /tmp/link.pcap --mac 02:00:5e:00:00:01
"""
import argparse
import socket
import struct
import sys
import time
import urllib.request

LINK_ETHERTYPE = 0x88B5
ETH_HDR = 14
LINK_HDR = 16
OP = {"HELLO": 1, "REQUEST": 2, "RESPONSE": 3, "DATA": 4, "EOF": 5, "ACK": 6, "WINDOW": 7}
OPNAME = {v: k for k, v in OP.items()}


class FrameStream:
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


class Pcap:
    def __init__(self, path):
        self.f = open(path, "wb")
        self.f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))

    def write(self, frame):
        t = time.time()
        self.f.write(struct.pack("<IIII", int(t), int((t % 1) * 1e6), len(frame), len(frame)))
        self.f.write(frame)
        self.f.flush()


def mac_str(b):
    return ":".join("%02x" % x for x in b)


def parse_mac(s):
    return bytes(int(x, 16) for x in s.split(":"))


FRAME_PAGES = 6   # 最大 Ethernet フレームが占める SRAM ページ数 (保守的な見積り)


class HostAgent:
    def __init__(self, mac, epoch=1, quiet=False):
        self.mac = mac
        self.epoch = epoch
        self.quiet = quiet
        self.hello_count = 0
        self.request_count = 0
        self.ack_count = 0
        self.window_count = 0
        # bulk (L1) 状態
        self.bulk = None   # dict: os32, count, plen, sent, acked, credit, eof_sent, max_inflight
        # stream (L2) 状態
        self.stream = None

    def log(self, *a):
        if not self.quiet:
            print(*a, flush=True)

    def build(self, dst, op, seq, ack, payload=b""):
        hdr = struct.pack("<BBHIIHH", op, 0, self.epoch, seq, ack, len(payload), 0)
        frame = dst + self.mac + struct.pack(">H", LINK_ETHERTYPE) + hdr + payload
        if len(frame) < 60:
            frame += b"\x00" * (60 - len(frame))
        return frame

    def handle(self, frame, stream, pcap):
        if len(frame) < ETH_HDR + LINK_HDR:
            return
        dst, src, et = frame[:6], frame[6:12], struct.unpack(">H", frame[12:14])[0]
        if et != LINK_ETHERTYPE:
            return
        op, _flags, epoch, seq, ack, plen, _r = struct.unpack("<BBHIIHH", frame[ETH_HDR:ETH_HDR + LINK_HDR])
        payload = frame[ETH_HDR + LINK_HDR:ETH_HDR + LINK_HDR + plen]
        if pcap:
            pcap.write(frame)

        if op == OP["HELLO"]:
            self.hello_count += 1
            self.epoch = max(self.epoch, epoch)
            reply = self.build(src, OP["HELLO"], 0, 0, self.mac)
            stream.send(reply)
            self.log("HELLO from %s -> reply (epoch %d)" % (mac_str(src), self.epoch))
        elif op == OP["REQUEST"]:
            self.request_count += 1
            if payload[:5] == b"BULK ":
                self._start_bulk(src, payload)
            elif payload[:7] == b"STREAM ":
                self._start_stream(src, payload)
            elif payload[:4] == b"GET ":
                self._service_get(src, seq, payload[4:].decode("latin1").strip(), stream)
            elif payload[:4] == b"TIME":
                ans = time.strftime("%Y-%m-%dT%H:%M:%S").encode()
                stream.send(self.build(src, OP["RESPONSE"], seq, seq, ans))
                self.log("TIME -> %s" % ans.decode())
            else:
                body = b"PONG " + payload
                stream.send(self.build(src, OP["RESPONSE"], seq, seq, body[:1484]))
                self.log("REQUEST seq=%d len=%d -> RESPONSE ack=%d" % (seq, plen, seq))
        elif op == OP["WINDOW"]:
            self.window_count += 1
            credit = struct.unpack("<H", payload[:2])[0] if len(payload) >= 2 else 0
            self._on_window(ack, credit, stream)
        elif op == OP["ACK"]:
            self.ack_count += 1
        else:
            self.log("%s seq=%d (ignored)" % (OPNAME.get(op, "op%d" % op), seq))

    def _start_bulk(self, os32, payload):
        try:
            _, cnt, plen = payload.split(b" ")[:3]
            count, plen = int(cnt), int(plen)
        except ValueError:
            self.log("bad BULK request: %r" % payload)
            return
        self.stream = None
        self.bulk = {"os32": os32, "count": count, "plen": plen, "sent": 0,
                     "acked": 0, "credit": 0, "eof_sent": False, "max_inflight": 0}
        self.log("BULK request: %d frames x %d bytes -> streaming under credit" % (count, plen))

    def _on_window(self, ack, credit, stream):
        if self.bulk:
            b = self.bulk
            b["acked"] = max(b["acked"], ack)
            b["credit"] = credit
            self._pump_bulk(stream)
        if self.stream:
            self._on_window_stream(ack, credit, stream)

    def _service_get(self, os32, seq, resource, stream):
        """L3 GET: ホストが実処理して RESPONSE(status,len) を返し、本文を DATA で流す。
        OS32 側は現代のスタック (HTTP/TLS/File) を持たず、結果だけ受け取る。"""
        status, body, gen = 200, b"", None
        if resource.startswith("/pattern/"):
            try:
                n = int(resource[len("/pattern/"):])
            except ValueError:
                n = 0
            # 本文はホスト RAM に貯めず、offset から生成する (巨大でも定数メモリ)
            gen, total = (lambda off, ln: bytes((off + k) & 0xFF for k in range(ln))), n
        elif resource.startswith("http://") or resource.startswith("https://"):
            try:
                with urllib.request.urlopen(resource, timeout=10) as r:
                    body = r.read()
                    status = getattr(r, "status", 200) or 200
            except Exception as e:
                status, body = 502, b""
                self.log("GET %s failed: %s" % (resource, e))
            total = len(body)
        elif resource.startswith("/file/"):
            try:
                with open(resource[len("/file/"):], "rb") as fh:
                    body = fh.read()
            except OSError:
                status, body = 404, b""
            total = len(body)
        else:
            status, total = 404, 0

        stream.send(self.build(os32, OP["RESPONSE"], seq, seq, struct.pack("<HI", status, total)))
        self.log("GET %s -> %d, %d bytes" % (resource, status, total))
        if status == 200 and total > 0:
            # 本文を L2 のストリーム機構 (WINDOW/Credit + Go-Back-N) で配送する
            plen = 512
            nframes = (total + plen - 1) // plen
            self.bulk = None
            self.stream = {"os32": os32, "total": total, "plen": plen, "dropseq": 0,
                           "nframes": nframes, "sent": 0, "acked": 0, "credit": 0,
                           "eof_sent": False, "dropped": False, "stall": 0, "max_inflight": 0,
                           "retx": 0, "body": body, "gen": gen}

    def _start_stream(self, os32, payload):
        try:
            _, tot, plen, drop = payload.split(b" ")[:4]
            total, plen, dropseq = int(tot), int(plen), int(drop)
        except ValueError:
            self.log("bad STREAM request: %r" % payload)
            return
        nframes = (total + plen - 1) // plen
        self.bulk = None
        self.stream = {"os32": os32, "total": total, "plen": plen, "dropseq": dropseq,
                       "nframes": nframes, "sent": 0, "acked": 0, "credit": 0,
                       "eof_sent": False, "dropped": False, "stall": 0, "max_inflight": 0, "retx": 0}
        self.log("STREAM request: %d bytes (%d frames x %d), drop seq %d" % (total, nframes, plen, dropseq))

    def _on_window_stream(self, ack, credit, stream):
        s = self.stream
        if ack > s["acked"]:
            s["acked"] = ack
            s["stall"] = 0
        else:
            s["stall"] += 1
            # ack が進まない = 欠落。Go-Back-N で acked+1 から再送する。
            if s["stall"] >= 8 and s["sent"] > s["acked"]:
                s["retx"] += s["sent"] - s["acked"]
                s["sent"] = s["acked"]
                s["stall"] = 0
        s["credit"] = credit
        self._pump_stream(stream)

    def _pump_stream(self, stream):
        s = self.stream
        while s["sent"] < s["nframes"]:
            inflight = FRAME_PAGES * (s["sent"] - s["acked"])
            if inflight + FRAME_PAGES > s["credit"]:
                break
            seq = s["sent"] + 1
            if seq == s["dropseq"] and not s["dropped"]:
                s["dropped"] = True          # 1 回だけ落とす (Go-Back-N が回復する)
                s["sent"] = seq
                continue
            off = (seq - 1) * s["plen"]
            ln = min(s["plen"], s["total"] - off)
            if s.get("gen") is not None:              # 生成 (パターン): ホスト RAM に貯めない
                body = s["gen"](off, ln)
            elif s.get("body") is not None:           # 実データ (HTTP/File): ホスト RAM から
                body = s["body"][off:off + ln]
            else:
                body = bytes((off + k) & 0xFF for k in range(ln))
            stream.send(self.build(s["os32"], OP["DATA"], seq, 0, body))
            s["sent"] = seq
            s["max_inflight"] = max(s["max_inflight"], FRAME_PAGES * (s["sent"] - s["acked"]))
        if s["sent"] == s["nframes"] and not s["eof_sent"]:
            stream.send(self.build(s["os32"], OP["EOF"], s["nframes"] + 1, 0, b""))
            s["eof_sent"] = True
            self.log("STREAM done: %d frames, max in-flight %d pages, retransmit %d"
                     % (s["nframes"], s["max_inflight"], s["retx"]))

    def _pump_bulk(self, stream):
        b = self.bulk
        if not b:
            return
        while b["sent"] < b["count"]:
            inflight = FRAME_PAGES * (b["sent"] - b["acked"])
            if inflight + FRAME_PAGES > b["credit"]:
                break                       # credit を超えるので待つ (絶対値 WINDOW を守る)
            b["sent"] += 1
            body = bytes((b["sent"] + k) & 0xFF for k in range(b["plen"]))
            stream.send(self.build(b["os32"], OP["DATA"], b["sent"], 0, body))
            b["max_inflight"] = max(b["max_inflight"], FRAME_PAGES * (b["sent"] - b["acked"]))
        if b["sent"] == b["count"] and not b["eof_sent"]:
            stream.send(self.build(b["os32"], OP["EOF"], b["count"] + 1, 0, b""))
            b["eof_sent"] = True
            self.log("BULK done: %d frames sent, max in-flight %d pages" % (b["count"], b["max_inflight"]))


def open_stream(args):
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
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1:8026", metavar="HOST:PORT")
    ap.add_argument("--connect", metavar="HOST:PORT")
    ap.add_argument("--mac", default="02:00:5e:00:00:01")
    ap.add_argument("--pcap", metavar="FILE")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--once", action="store_true", help="exit after the peer disconnects")
    args = ap.parse_args()

    agent = HostAgent(parse_mac(args.mac), quiet=args.quiet)
    pcap = Pcap(args.pcap) if args.pcap else None
    print("host_agent mac %s (L0 HELLO + REQUEST/RESPONSE echo)" % args.mac, flush=True)

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
            print("disconnected: %s (hello=%d request=%d ack=%d)"
                  % (e, agent.hello_count, agent.request_count, agent.ack_count), flush=True)
            if args.once:
                return 0
            if args.connect:
                time.sleep(1.0)


if __name__ == "__main__":
    sys.exit(main())
