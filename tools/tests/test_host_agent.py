#!/usr/bin/env python3
"""N1 段 1: tools/host_agent.py v2 の TDD (TASK_N0 §3 の Agent 側項目)。

贋 OS32 は**フレームを直接組む** (Agent の build を借りない) ので、20B ヘッダの
直列化・op ごとの payload 長検査・sess / epoch の照合もここで踏む。
Agent は実物 (tools/host_agent.py の HostAgent) をそのまま import し、出口だけ
ListSink に差し替える。ネットワークにもエミュレータにも触らない。

  python3 -B tools/tests/test_host_agent.py [ケース名 ...]

ケース名は TASK_N0 §3 の指摘番号 (往復 2 = r2_*, 往復 3 = r3_*, 往復 4 = r4_*)
を頭に付ける。対応表は tools/tests/n1_tdd.md。
"""
import os
import pathlib
import random
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import host_agent as HA          # noqa: E402  (パスを通した後で読む)

OS32_MAC = bytes.fromhex("02005e000002")
AGENT_MAC = bytes.fromhex("02005e000001")


# ------------------------------------------------------------- 贋 OS32
class FakeOS32:
    """フレームを直接組む贋 OS32。Agent の直列化を借りない。"""

    def __init__(self, agent, mac=OS32_MAC):
        self.agent = agent
        self.mac = mac
        self.sink = HA.ListSink()
        self.sess = 0
        self.epoch = 1
        self.nonce = 0x1000

    # ---- 直列化 (host_agent.py と独立に組む) ----
    def frame(self, op, flags=0, epoch=None, seq=0, ack=0, rid=0, payload=b"",
              sess=None, dst=None, length=None):
        ep = self.epoch if epoch is None else epoch
        ss = self.sess if sess is None else sess
        ln = len(payload) if length is None else length
        hdr = (struct.pack("<BB", op & 0xFF, flags & 0xFF) +
               struct.pack("<H", ep & 0xFFFF) +
               struct.pack("<I", seq & 0xFFFFFFFF) +
               struct.pack("<I", ack & 0xFFFFFFFF) +
               struct.pack("<H", ln & 0xFFFF) +
               struct.pack("<I", rid & 0xFFFFFFFF) +
               struct.pack("<H", ss & 0xFFFF))
        assert len(hdr) == 20
        f = (dst or AGENT_MAC) + self.mac + struct.pack(">H", HA.LINK_ETHERTYPE) + hdr + payload
        if len(f) < 60:
            f += b"\x00" * (60 - len(f))
        return f

    def send(self, *a, **kw):
        """1 フレーム渡して、その結果 Agent が出したフレームを返す。"""
        self.sink.frames = []
        self.agent.handle(self.frame(*a, **kw), self.sink)
        return [parse(f) for f in self.sink.frames]

    # ---- 3 way HELLO ----
    def hello(self, sess=None, epoch=None, nonce=None):
        self.nonce = nonce if nonce is not None else self.nonce + 1
        if epoch is not None:
            self.epoch = epoch
        out = self.send(HA.OP_HELLO, HA.HS_SYN, epoch=self.epoch, seq=self.nonce,
                        sess=self.sess if sess is None else sess)
        if not out:
            return None
        sa = out[0]
        self.sess, self.epoch = sa["sess"], sa["epoch"]
        out2 = self.send(HA.OP_HELLO, HA.HS_CONFIRM, seq=self.nonce, ack=sa["ack"])
        return (sa, out2[0] if out2 else None)


def parse(frame):
    op, flags, epoch, seq, ack, plen, rid, sess = HA.hdr_unpack(frame[HA.ETH_HDR:])
    return {"op": op, "flags": flags, "epoch": epoch, "seq": seq, "ack": ack,
            "len": plen, "rid": rid, "sess": sess,
            "pl": frame[HA.ETH_HDR + HA.LINK_HDR:HA.ETH_HDR + HA.LINK_HDR + plen],
            "src": frame[6:12], "dst": frame[:6]}


def resp(p):
    """RESPONSE の (制御か?, status, length)。"""
    st, ln = struct.unpack("<HI", p["pl"])
    return (bool(p["flags"] & HA.F_CTRL), st, ln)


def new_agent(**kw):
    kw.setdefault("quiet", True)
    kw.setdefault("agent_gen", 0x4242)
    kw.setdefault("rng", random.Random(7))
    kw.setdefault("allow_net", False)
    return HA.HostAgent(AGENT_MAC, **kw)


def up(**kw):
    """HELLO 済みの (agent, os32) を作る。"""
    a = new_agent(**kw)
    o = FakeOS32(a)
    o.hello()
    return a, o


def only(out, op):
    got = [p for p in out if p["op"] == op]
    assert len(got) == 1, "op %d が %d 本 (全部: %s)" % (op, len(got),
                                                        [p["op"] for p in out])
    return got[0]


# =========================================================== ケース (往復 2)
def r2_R1_hello_and_roundtrip():
    """R1: 非ゼロの agent 世代での HELLO → REQUEST → RESPONSE → 本文完了。"""
    a, o = up()
    sa, est = o.hello(sess=o.sess)
    agent_gen, req_sess, req_epoch = struct.unpack("<HHH", sa["pl"])
    assert agent_gen == 0x4242 and agent_gen != 0, "agent 世代が載っていない"
    assert est["op"] == HA.OP_HELLO and est["flags"] == HA.HS_ESTAB
    assert a.established and a.sess == o.sess

    out = o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"GET /pattern/1024")
    ack = only(out, HA.OP_ACK)
    assert ack["rid"] == 1 and ack["ack"] == 0 and ack["flags"] == 0
    ctl, st, ln = resp(only(out, HA.OP_RESPONSE))
    assert (ctl, st, ln) == (False, 200, 1024), (ctl, st, ln)

    got = b""
    seq = 0
    for _ in range(20):
        out = o.send(HA.OP_WINDOW, rid=1, ack=seq, payload=struct.pack("<H", 64))
        data = [p for p in out if p["op"] == HA.OP_DATA]
        if not data and not [p for p in out if p["op"] == HA.OP_EOF]:
            break
        for d in data:
            assert d["seq"] == seq + 1
            seq = d["seq"]
            got += d["pl"]
        if [p for p in out if p["op"] == HA.OP_EOF]:
            break
    assert len(got) == 1024, len(got)
    assert got == bytes(k & 0xFF for k in range(1024)), "本文が pattern と違う"


def r2_R2_request_ack_dup_and_wdata_loss():
    """R2: REQUEST ACK の遅延重複 + WDATA 喪失を同時に注入しても壊れない。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=5, seq=0, payload=b"ECHO 6")
    o.send(HA.OP_REQUEST, rid=5, seq=0, payload=b"ECHO 6")     # 遅延重複
    assert a.ledger[5]["st"] == "ACTIVE" and a.ledger[5]["got"] == b""
    out = o.send(HA.OP_WDATA, rid=5, seq=2, payload=b"xyz")    # seq1 が落ちた体
    assert only(out, HA.OP_ACK)["ack"] == 0, "欠落しているのに累積 ACK が進んだ"
    out = o.send(HA.OP_WDATA, rid=5, seq=1, payload=b"abc")
    assert only(out, HA.OP_ACK)["ack"] == 1
    out = o.send(HA.OP_WDATA, rid=5, seq=2, payload=b"xyz")
    ctl, st, ln = resp(only(out, HA.OP_RESPONSE))
    assert (ctl, st, ln) == (False, 200, 6), (ctl, st, ln)
    assert a.ledger[5]["got"] == b"abcxyz"


def r2_R3_status_repeats_response_zero_len():
    """R3: 転送 ACK 後の RESPONSE 消失 → STATUS で再提示 (0 長も対象)。"""
    a, o = up()
    out = o.send(HA.OP_REQUEST, rid=3, seq=0, payload=b"PING")
    ctl, st, ln = resp(only(out, HA.OP_RESPONSE))
    assert (ctl, st, ln) == (False, 200, 0)
    out = o.send(HA.OP_STATUS, rid=3)                           # 応答が消えた体
    ctl, st, ln = resp(only(out, HA.OP_RESPONSE))
    assert (ctl, st, ln) == (False, 200, 0), "0 長の成功応答が再提示されない"


def r2_R4_release_drops_the_right_rid():
    """R4: A / B の close 順に関わらず RELEASE が正しい rid だけを捨てる。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=10, seq=0, payload=b"GET /pattern/512")
    o.send(HA.OP_REQUEST, rid=11, seq=0, payload=b"GET /pattern/512")
    assert a._active_count() == 2
    out = o.send(HA.OP_RELEASE, rid=11)                          # B を先に閉じる
    ack = only(out, HA.OP_ACK)
    assert ack["flags"] & HA.F_RELACK and ack["rid"] == 11 and ack["ack"] == 0
    assert a.ledger[11]["st"] == "RELEASED"
    assert a.ledger[10]["st"] == "ACTIVE", "A まで捨てた"
    out = o.send(HA.OP_STATUS, rid=10)
    assert resp(only(out, HA.OP_RESPONSE)) == (False, 200, 512)
    out = o.send(HA.OP_STATUS, rid=11)
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_TOMBSTONE, 0)


def r2_R5_longlived_a_survives_many_short_b():
    """R5: 長寿命 A + 多数の短命 B。A の結果は残り、8 件境界を越えた遅延
    REQUEST は TOMBSTONE で再実行されない。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"GET /pattern/256")   # 長寿命 A
    late = 2
    o.send(HA.OP_REQUEST, rid=late, seq=0, payload=b"PING")
    o.send(HA.OP_RELEASE, rid=late)                                    # B を 1 本閉じる
    for rid in range(3, 24):
        o.send(HA.OP_REQUEST, rid=rid, seq=0, payload=b"PING")
        o.send(HA.OP_RELEASE, rid=rid)
    assert a.ledger[1]["st"] == "ACTIVE", "長寿命 A が watermark で落とされた"
    assert resp(only(o.send(HA.OP_STATUS, rid=1), HA.OP_RESPONSE)) == (False, 200, 256)
    assert late not in a.ledger, "8 件境界を越えた墓標が残っている"
    out = o.send(HA.OP_REQUEST, rid=late, seq=0, payload=b"PING")      # 遅延 REQUEST
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_TOMBSTONE, 0), \
        "落とした墓標の rid が再実行された"


def r2_R7_stale_hello_and_stale_response():
    """R7: 旧 sess / 旧 epoch のフレームは現行セッションに混ざらない。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"PING")
    old_sess = o.sess
    out = o.send(HA.OP_STATUS, rid=1, sess=old_sess + 7)
    assert out == [], "別 sess のフレームに答えた"
    out = o.send(HA.OP_STATUS, rid=1, epoch=o.epoch + 5)
    assert out == [], "別 epoch のフレームに答えた"
    out = o.send(HA.OP_STATUS, rid=1)
    assert len(out) == 1, "現行セッションのフレームまで捨てた"


# =========================================================== ケース (往復 3)
def r3_B1_release_ack_vs_request_ack():
    """B1: RELEASE の ACK は flags bit0。REQUEST の ACK (flags 0) と識別できる。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=4, seq=0, payload=b"PING")
    out = o.send(HA.OP_REQUEST, rid=4, seq=0, payload=b"PING")   # 旧 REQUEST の再送
    ack = only(out, HA.OP_ACK)
    assert ack["flags"] & HA.F_RELACK == 0, "転送 ACK に RELEASE の印が付いた"
    out = o.send(HA.OP_RELEASE, rid=4)
    assert only(out, HA.OP_ACK)["flags"] & HA.F_RELACK, "RELEASE の ACK に印が無い"


def r3_B2_release_before_and_after_request():
    """B2: RELEASE 直後の同 rid の遅延 REQUEST は TOMBSTONE。
    RELEASE が REQUEST に先着しても後着の REQUEST を止める。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=6, seq=0, payload=b"PING")
    o.send(HA.OP_RELEASE, rid=6)
    out = o.send(HA.OP_REQUEST, rid=6, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_TOMBSTONE, 0)
    # RELEASE 先着 (規則 (4))
    out = o.send(HA.OP_RELEASE, rid=9)
    assert only(out, HA.OP_ACK)["flags"] & HA.F_RELACK
    out = o.send(HA.OP_REQUEST, rid=9, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_TOMBSTONE, 0), \
        "RELEASE 先着の rid が後から受理された"


def r3_B3_same_clock_restart_is_a_new_session():
    """B3: 同じ RTC 秒・同じ初期 tick で OS32 が再起動しても別セッション
    (sess は Agent 採番なので OS32 側の種に依らない)。"""
    a = new_agent()
    o1 = FakeOS32(a)
    o1.hello(sess=0, epoch=1, nonce=0x1000)
    s1 = o1.sess
    o2 = FakeOS32(a)                                  # 再起動 (nonce も epoch も同じ)
    o2.hello(sess=0, epoch=1, nonce=0x1000)
    assert o2.sess != s1, "再起動が同じ sess を貰った (%d)" % s1
    assert a.sess == o2.sess and a.ledger == {}, "旧セッションの台帳が残った"


def r3_B4_delayed_old_syn_cannot_break_session():
    """B4: 遅延した旧 sess の SYN は SYN-ACK (候補) を作るだけで切替が起きず、
    旧 CONFIRM は Agent nonce 不一致で無視される。"""
    a, o = up()
    live_sess, live_epoch = o.sess, o.epoch
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"PING")

    ghost = FakeOS32(a, mac=bytes.fromhex("02005e0000ff"))
    out = ghost.send(HA.OP_HELLO, HA.HS_SYN, epoch=9, seq=0x777, sess=0)
    assert out and out[0]["flags"] == HA.HS_SYNACK
    assert a.sess == live_sess and a.epoch == live_epoch, "SYN-ACK だけで切り替わった"
    # 旧 CONFIRM (別の agent nonce) を投げても切り替わらない
    out = ghost.send(HA.OP_HELLO, HA.HS_CONFIRM, epoch=9, seq=0x777, ack=0xDEAD,
                     sess=out[0]["sess"])
    assert out == [], "nonce 不一致の CONFIRM に答えた"
    assert a.sess == live_sess, "nonce 不一致の CONFIRM で切り替わった"
    assert a.ledger[1]["st"] == "ACTIVE", "現行セッションの受付が消えた"


def r3_B5_epoch_wrap_uses_a_new_session():
    """B5: epoch 65535 の次は sess=0 の SYN (新セッション) で成立する。
    Agent は epoch の大小で HELLO を拒まない。"""
    a = new_agent()
    o = FakeOS32(a)
    o.hello(sess=0, epoch=HA.EPOCH_MAX)
    assert a.epoch == HA.EPOCH_MAX and a.established
    old = o.sess
    o2 = FakeOS32(a)
    o2.hello(sess=0, epoch=1)                        # 周回して新セッション
    assert a.established and a.epoch == 1 and o2.sess != old


def r3_B6_epoch_bump_frees_active_slots():
    """B6: 同 sess の epoch 更新で ACTIVE 2 件が消え、新要求が NO_SLOT に
    ならない。墓標は保たれる (rid はセッション内で単調なので有効)。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"PING")
    o.send(HA.OP_RELEASE, rid=1)                                # 墓標を 1 本作る
    o.send(HA.OP_REQUEST, rid=2, seq=0, payload=b"GET /pattern/64")
    o.send(HA.OP_REQUEST, rid=3, seq=0, payload=b"GET /pattern/64")
    assert a._active_count() == 2
    o.hello(sess=o.sess, epoch=o.epoch + 1)                     # 同 sess の再同期
    assert a._active_count() == 0, "epoch 更新で ACTIVE が残った"
    assert a.ledger.get(1, {}).get("st") == "RELEASED", "墓標まで捨てた"
    out = o.send(HA.OP_REQUEST, rid=4, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (False, 200, 0), "新要求が NO_SLOT"


def r3_B6_hole_becomes_tombstone_on_epoch_bump():
    """B6 の続き: epoch 切替で HOLE は RELEASED に変わる (埋まらない穴を残さない)。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=5, seq=0, payload=b"PING")        # rid 1〜4 が HOLE
    assert [r for r, e in a.ledger.items() if e["st"] == "HOLE"] == [1, 2, 3, 4]
    o.hello(sess=o.sess, epoch=o.epoch + 1)
    assert all(e["st"] != "HOLE" for e in a.ledger.values()), "HOLE が残った"
    out = o.send(HA.OP_REQUEST, rid=2, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_TOMBSTONE, 0)


def r3_B7_business_status_not_confused_with_control():
    """B7: HTTP 503 / 本文付き 410 は業務結果 (flags 0) として届き、
    制御の NO_SLOT / TOMBSTONE (flags bit0) と混ざらない。"""
    a, o = up()
    out = o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"GET /status/503")
    assert resp(only(out, HA.OP_RESPONSE)) == (False, 503, 0)
    out = o.send(HA.OP_REQUEST, rid=2, seq=0, payload=b"GET /status/410")
    ctl, st, ln = resp(only(out, HA.OP_RESPONSE))
    assert (ctl, st) == (False, 410) and ln == len(b"gone body"), (ctl, st, ln)
    # 制御側 (NO_SLOT) は同じ 3 という数字でも flags bit0 で分かれる
    out = o.send(HA.OP_REQUEST, rid=3, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_NO_SLOT, 0)


def r3_no_slot_keeps_a_hole():
    """規則 (1): 枠が無いときの REQUEST は NO_SLOT を返し、その rid は
    墓標ではなく HOLE として残る (再送が (2') で受理される)。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"GET /pattern/64")
    o.send(HA.OP_REQUEST, rid=2, seq=0, payload=b"GET /pattern/64")
    out = o.send(HA.OP_REQUEST, rid=3, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (True, HA.CTL_NO_SLOT, 0)
    assert a.ledger[3]["st"] == "HOLE", a.ledger[3]
    o.send(HA.OP_RELEASE, rid=1)                                 # 枠を空けて再送
    out = o.send(HA.OP_REQUEST, rid=3, seq=0, payload=b"PING")
    assert resp(only(out, HA.OP_RESPONSE)) == (False, 200, 0), "再送が受理されない"


# =========================================================== ケース (往復 4)
def r4_R1_hole_accepts_the_retransmitted_request():
    """R1: A の REQUEST 初回欠落 → B 受理 → A 再送が **新規** として受理される
    (未受理の穴を墓標にしない)。"""
    a, o = up()
    # rid 1 (A) は落ちた。B = rid 2 が先に着く → rid 1 は HOLE
    o.send(HA.OP_REQUEST, rid=2, seq=0, payload=b"GET /pattern/64")
    assert a.ledger[1]["st"] == "HOLE"
    out = o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"GET /pattern/32")
    assert resp(only(out, HA.OP_RESPONSE)) == (False, 200, 32), "穴が墓標になっていた"
    assert a.ledger[1]["st"] == "ACTIVE"


def r4_R2_restart_does_not_fall_back_to_old_session():
    """R2: 再起動 (req_sess = 0) の SYN と、旧セッションの再同期 SYN (req_sess = S)
    の応答が先後逆転しても、写しの照合で旧セッションへ戻らない。"""
    a, o = up()
    old = o.sess
    # 旧起動が「セッション内の再同期」の SYN を出していた (nonce N)
    n = 0x5555
    out = o.send(HA.OP_HELLO, HA.HS_SYN, epoch=o.epoch + 1, seq=n, sess=old)
    sa_resync = out[0]
    assert struct.unpack("<HHH", sa_resync["pl"])[1] == old, "req_sess の写しが無い"
    # そこへ OS32 が再起動し、同じ nonce で新セッションの SYN を出す
    o2 = FakeOS32(a)
    out = o2.send(HA.OP_HELLO, HA.HS_SYN, epoch=1, seq=n, sess=0)
    sa_new = out[0]
    gen, req_sess, req_epoch = struct.unpack("<HHH", sa_new["pl"])
    assert req_sess == 0 and req_epoch == 1, (req_sess, req_epoch)
    assert sa_new["sess"] != old, "再起動に旧 sess を割り当てた"
    # 遅延していた再同期用 SYN-ACK は req_sess = old なので再起動側は採用しない。
    # Agent 側は候補が 1 件なので、旧 SYN-ACK の CONFIRM は nonce が合っても
    # 候補の sess / epoch と合わず無視される。
    out = o2.send(HA.OP_HELLO, HA.HS_CONFIRM, epoch=sa_resync["epoch"], seq=n,
                  ack=sa_resync["ack"], sess=sa_resync["sess"])
    assert out == [], "旧セッションの CONFIRM を受理した"
    assert a.sess == old, "無視すべき CONFIRM でセッションが動いた"
    # 再起動側が自分の SYN-ACK の写しで CONFIRM すると、新 sess で成立する
    o2.sess, o2.epoch = sa_new["sess"], sa_new["epoch"]
    out = o2.send(HA.OP_HELLO, HA.HS_CONFIRM, seq=n, ack=sa_new["ack"])
    assert out and out[0]["flags"] == HA.HS_ESTAB
    assert a.sess == sa_new["sess"] != old, "再起動後に旧セッションへ戻った"
    assert a.ledger == {}, "新セッションに旧セッションの台帳が残った"


def r4_R3_sess_exhaustion_stops_the_agent():
    """R3: sess が枯渇したら新セッションの SYN に応答せず止まる。
    旧 (sess, epoch, rid) のフレームを注いでも新しい要求は成立しない。"""
    with tempfile.TemporaryDirectory(prefix="os32-sess-") as d:
        with open(os.path.join(d, "sess.txt"), "w") as f:
            f.write("%d\n" % (HA.SESS_MAX - 1))
        a = new_agent(state_dir=d)
        o1 = FakeOS32(a)
        o1.hello(sess=0, epoch=1)
        assert o1.sess == HA.SESS_MAX, o1.sess
        o1.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"PING")
        o2 = FakeOS32(a, mac=bytes.fromhex("02005e0000ee"))
        out = o2.send(HA.OP_HELLO, HA.HS_SYN, epoch=1, seq=1, sess=0)
        assert out == [] and a.stopped, "枯渇後も新セッションに応答した"
        # 旧 (sess, epoch, rid) を騙っても、別の OS32 は現行セッションに乗れない
        out = o2.send(HA.OP_REQUEST, rid=2, seq=0, payload=b"PING",
                      sess=HA.SESS_MAX, epoch=o1.epoch)
        assert out == [], "旧セッション偽装に答えた"
        assert 2 not in a.ledger, "偽装した rid が台帳に入った"
        # 生きている旧セッションはそのまま動く (枯渇は新規受付だけを止める)
        out = o1.send(HA.OP_STATUS, rid=1)
        assert resp(only(out, HA.OP_RESPONSE)) == (False, 200, 0)
        # sess.txt は再起動をまたいで残る
        a2 = new_agent(state_dir=d)
        assert a2.sessions.exhausted()


def r4_sess_not_reused_across_restart():
    """sess は Agent 再起動をまたいでも再使用しない (永続採番)。"""
    with tempfile.TemporaryDirectory(prefix="os32-sess-") as d:
        a = new_agent(state_dir=d)
        o = FakeOS32(a)
        o.hello(sess=0, epoch=1)
        first = o.sess
        a2 = new_agent(state_dir=d)                      # Agent 再起動
        o2 = FakeOS32(a2)
        o2.hello(sess=0, epoch=1)
        assert o2.sess == first + 1, (first, o2.sess)


def r4_hello_stage_losses():
    """HELLO 各段階の消失: SYN / SYN-ACK / CONFIRM / ESTABLISHED。
    再送は冪等で、同じ sess・同じ Agent nonce が返る。"""
    a = new_agent()
    o = FakeOS32(a)
    # SYN が落ちた = Agent は何も見ていない
    assert not a.established
    # SYN-ACK が落ちた → OS32 は **nonce を +1 して** SYN を再送する。
    # 候補は最新の SYN で上書きされ、古い SYN-ACK の CONFIRM はもう通らない。
    out1 = o.send(HA.OP_HELLO, HA.HS_SYN, epoch=1, seq=0x99, sess=0)
    out2 = o.send(HA.OP_HELLO, HA.HS_SYN, epoch=1, seq=0x9A, sess=0)
    assert out1[0]["ack"] != out2[0]["ack"], "SYN 再送で Agent nonce が変わらない"
    o.sess, o.epoch = out1[0]["sess"], out1[0]["epoch"]
    assert o.send(HA.OP_HELLO, HA.HS_CONFIRM, seq=0x99, ack=out1[0]["ack"]) == [], \
        "上書きされた候補の CONFIRM が通った"
    # CONFIRM が落ちた / ESTABLISHED が落ちた → 同じ CONFIRM を再送すると
    # Agent は冪等に ESTABLISHED を返す
    o.sess, o.epoch = out2[0]["sess"], out2[0]["epoch"]
    e1 = o.send(HA.OP_HELLO, HA.HS_CONFIRM, seq=0x9A, ack=out2[0]["ack"])
    e2 = o.send(HA.OP_HELLO, HA.HS_CONFIRM, seq=0x9A, ack=out2[0]["ack"])
    assert e1[0]["flags"] == HA.HS_ESTAB and e2[0]["flags"] == HA.HS_ESTAB, \
        "重複 CONFIRM に冪等な ESTABLISHED を返さない"
    assert a.established and a.sess == o.sess


# ============================================================== その他
def paylen_mismatch_is_dropped():
    """op ごとの payload 長と一致しないフレームは捨てる (v2 パーサの検査)。"""
    a, o = up()
    before = dict(a.counts)
    o.send(HA.OP_ACK, rid=1, payload=b"xx")                  # ACK は 0B
    o.send(HA.OP_WINDOW, rid=1, payload=b"x")                # WINDOW は 2B
    o.send(HA.OP_STATUS, rid=1, payload=b"x")                # STATUS は 0B
    o.send(HA.OP_RELEASE, rid=1, payload=b"x")               # RELEASE は 0B
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"")         # REQUEST は 1B 以上
    assert a.counts["dropped"] - before["dropped"] == 5, a.counts
    assert a.ledger == {}, "捨てたはずのフレームが台帳を動かした"
    # 宣言長がフレームに収まらない (length だけ大きい)
    out = o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"PING", length=900)
    assert out == [] and a.ledger == {}


def hello_paylen_per_stage():
    """HELLO は段階ごとに payload 長が決まっている (SYN 0 / SYN-ACK 6 /
    CONFIRM 0 / ESTABLISHED 2)。違うものは捨てる。"""
    a = new_agent()
    o = FakeOS32(a)
    assert o.send(HA.OP_HELLO, HA.HS_SYN, epoch=1, seq=1, sess=0,
                  payload=b"xx") == [], "SYN に payload を付けても通った"
    assert not a.established
    out = o.send(HA.OP_HELLO, HA.HS_SYN, epoch=1, seq=1, sess=0)
    assert out and out[0]["len"] == 6, "SYN-ACK の payload が 6B でない"


def wdata_dedup_by_sess_rid_seq():
    """WDATA の重複排除は (sess, rid, seq)。重複は本文を二重に足さない。"""
    a, o = up()
    o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"ECHO 4")
    o.send(HA.OP_WDATA, rid=1, seq=1, payload=b"ab")
    o.send(HA.OP_WDATA, rid=1, seq=1, payload=b"ab")          # 重複
    assert a.ledger[1]["got"] == b"ab", a.ledger[1]["got"]
    out = o.send(HA.OP_WDATA, rid=1, seq=2, payload=b"cd")
    assert resp(only(out, HA.OP_RESPONSE)) == (False, 200, 4)
    assert a.ledger[1]["got"] == b"abcd"


def window_gates_delivery():
    """WINDOW を受けるまで DATA は 1 本も流れない (配送開始の許可)。"""
    a, o = up()
    out = o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"GET /pattern/4096")
    assert [p for p in out if p["op"] == HA.OP_DATA] == [], "WINDOW 前に流れた"
    out = o.send(HA.OP_WINDOW, rid=1, ack=0, payload=struct.pack("<H", 12))
    data = [p for p in out if p["op"] == HA.OP_DATA]
    assert len(data) == 2, "credit 12 ページ (6 ページ/フレーム) で %d 本" % len(data)
    out = o.send(HA.OP_WINDOW, rid=1, ack=0, payload=struct.pack("<H", 12))
    assert [p for p in out if p["op"] == HA.OP_DATA] == [], "credit を超えて流れた"
    out = o.send(HA.OP_WINDOW, rid=2, ack=0, payload=struct.pack("<H", 99))
    assert [p for p in out if p["op"] == HA.OP_DATA] == [], "別 rid の WINDOW で流れた"


def time_format_is_19_bytes():
    """TIME の標準形は `YYYY-MM-DD HH:MM:SS` (19B、HOST_SERVICES_PLAN §2)。"""
    a, o = up()
    out = o.send(HA.OP_REQUEST, rid=1, seq=0, payload=b"TIME")
    ctl, st, ln = resp(only(out, HA.OP_RESPONSE))
    assert (ctl, st, ln) == (False, 200, 19), (ctl, st, ln)


CASES = [
    r2_R1_hello_and_roundtrip,
    r2_R2_request_ack_dup_and_wdata_loss,
    r2_R3_status_repeats_response_zero_len,
    r2_R4_release_drops_the_right_rid,
    r2_R5_longlived_a_survives_many_short_b,
    r2_R7_stale_hello_and_stale_response,
    r3_B1_release_ack_vs_request_ack,
    r3_B2_release_before_and_after_request,
    r3_B3_same_clock_restart_is_a_new_session,
    r3_B4_delayed_old_syn_cannot_break_session,
    r3_B5_epoch_wrap_uses_a_new_session,
    r3_B6_epoch_bump_frees_active_slots,
    r3_B6_hole_becomes_tombstone_on_epoch_bump,
    r3_B7_business_status_not_confused_with_control,
    r3_no_slot_keeps_a_hole,
    r4_R1_hole_accepts_the_retransmitted_request,
    r4_R2_restart_does_not_fall_back_to_old_session,
    r4_R3_sess_exhaustion_stops_the_agent,
    r4_sess_not_reused_across_restart,
    r4_hello_stage_losses,
    paylen_mismatch_is_dropped,
    hello_paylen_per_stage,
    wdata_dedup_by_sess_rid_seq,
    window_gates_delivery,
    time_format_is_19_bytes,
]


def main():
    want = [x for x in sys.argv[1:] if not x.startswith("--")]
    cases = [c for c in CASES if not want or c.__name__ in want]
    failed = 0
    for c in cases:
        try:
            c()
            print("  ok   %s" % c.__name__, flush=True)
        except AssertionError as e:
            print("  FAIL %s: %s" % (c.__name__, e), flush=True)
            failed += 1
        except Exception as e:                       # noqa: BLE001
            print("  ERROR %s: %r" % (c.__name__, e), flush=True)
            failed += 1
    print("SUMMARY %d/%d PASS" % (len(cases) - failed, len(cases)), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
