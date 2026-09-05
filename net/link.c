/* ======================================================================== */
/*  LINK.C — OS32 リンクプロトコル (L0: Stop-and-Wait)                       */
/*                                                                          */
/*  計画: docs/tasks/network/LINK_PLAN.md §4 / §5 (L0)。                     */
/*  ドライバ API (drivers/ne2000.c) だけに依存し、NIC の詳細は持たない。      */
/*  L0 は同期的な HELLO 交換と REQUEST/RESPONSE の 1 往復 (ACK まで次を出さ   */
/*  ない)。IF=1 の文脈で使う (tick_count でタイムアウトを数える)。            */
/* ======================================================================== */

#include "link.h"
#include "ne2000.h"
#include "kstring.h"
#include "kprintf.h"
#include "idt.h"            /* tick_count */
#include "cpu_calibrate.h"  /* cpu_delay_us */

/* ---- タイムアウト・再送 (100Hz tick 基準) ---- */
#define LINK_RTO_TICKS      20      /* 1 往復の待ち上限 (200ms)。実機余裕は後で調整 */
#define LINK_POLL_BUDGET    4
#define LINK_PING_LEN       16

static const u8 link_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static u8  link_my_mac[6];
static int link_ready = 0;      /* link_init 済み */

static u8  link_txbuf[LINK_ETH_HDR_LEN + LINK_HDR_LEN + LINK_MAX_PAYLOAD];
static u8  link_rxbuf[1520];

static u32 link_tx_seq = 0;     /* 次に送る REQUEST の seq */
static u32 link_rx_ack = 0;     /* 相手へ返す ack (最後に受けた in-order seq) */

/* 直近に受けた RESPONSE (link_request がここから拾う) */
static int link_last_resp_valid = 0;
static u32 link_last_resp_seq = 0;
static u32 link_last_resp_ack = 0;
static u16 link_last_resp_len = 0;
static u8  link_last_resp[LINK_MAX_PAYLOAD];

/* 観測用カウンタ */
u32 link_hello_ok = 0;
u32 link_rt_ok = 0;
u32 link_rt_fail = 0;
u32 link_retransmits = 0;
u32 link_rx_frames = 0;
u32 link_rx_dropped = 0;
u8  link_peer_mac[6] = { 0, 0, 0, 0, 0, 0 };
u16 link_epoch = 0;

/* ---- 小さな LE アクセサ (受信フレームは非整列なのでバイト単位で読む) ---- */
static int mac_eq(const u8 *a, const u8 *b)
{
    int i;
    for (i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

void link_init(const u8 my_mac[6])
{
    kmemcpy(link_my_mac, my_mac, 6);
    link_ready = 1;
    link_tx_seq = 1;
    link_rx_ack = 0;
    link_epoch = 1;
    link_hello_ok = 0;
    link_last_resp_valid = 0;
    kmemset(link_peer_mac, 0, 6);
}

/* op / seq / ack / payload を組んで 1 フレーム送る。dst は peer (未確立時は bcast)。 */
static int link_send(u8 op, const u8 *dst, u32 seq, u32 ack,
                     const void *payload, unsigned int len)
{
    u8 *p = link_txbuf;
    unsigned int total;

    if (len > LINK_MAX_PAYLOAD) return LINK_ERR_TOOBIG;
    kmemcpy(p, dst, 6);
    kmemcpy(p + 6, link_my_mac, 6);
    /* EtherType はワイヤ上 big-endian。リンクヘッダ以降は自前プロトコルなので LE。 */
    p[12] = (u8)(LINK_ETHERTYPE >> 8);
    p[13] = (u8)(LINK_ETHERTYPE & 0xFF);
    p[14] = op;
    p[15] = 0;                          /* flags */
    wr16(p + 16, link_epoch);
    wr32(p + 18, seq);
    wr32(p + 22, ack);
    wr16(p + 26, (u16)len);
    wr16(p + 28, 0);                    /* rsvd */
    if (len) kmemcpy(p + 30, payload, len);
    total = LINK_ETH_HDR_LEN + LINK_HDR_LEN + len;
    return (ne2k_send(p, total) == NE2K_OK) ? LINK_OK : LINK_ERR;
}

/* 受信フレームを 1 つ処理する。自分宛 (または broadcast) の LINK_ETHERTYPE だけ。 */
static void link_dispatch(const u8 *f, unsigned int flen)
{
    const u8 *h;
    u8 op;
    u16 plen;
    u32 seq, ack;

    if (flen < LINK_ETH_HDR_LEN + LINK_HDR_LEN) { link_rx_dropped++; return; }
    if ((((u16)f[12] << 8) | f[13]) != LINK_ETHERTYPE) { link_rx_dropped++; return; }  /* EtherType は big-endian */
    /* 宛先が自分か broadcast か (P2P なので基本は自分宛) */
    if (!mac_eq(f, link_my_mac) && !mac_eq(f, link_bcast)) {
        link_rx_dropped++;
        return;
    }
    h = f + LINK_ETH_HDR_LEN;
    op = h[0];
    seq = rd32(h + 4);
    ack = rd32(h + 8);
    plen = rd16(h + 12);
    if ((unsigned int)(LINK_ETH_HDR_LEN + LINK_HDR_LEN + plen) > flen) { link_rx_dropped++; return; }
    link_rx_frames++;

    switch (op) {
    case LINK_OP_HELLO:
        /* peer MAC を送信元から学ぶ。epoch は大きい方に合わせる。 */
        kmemcpy(link_peer_mac, f + 6, 6);
        if (rd16(h + 2) > link_epoch) link_epoch = rd16(h + 2);
        link_hello_ok = 1;
        break;
    case LINK_OP_RESPONSE:
        link_last_resp_seq = seq;
        link_last_resp_ack = ack;
        link_last_resp_len = (plen > LINK_MAX_PAYLOAD) ? LINK_MAX_PAYLOAD : plen;
        if (link_last_resp_len) kmemcpy(link_last_resp, h + LINK_HDR_LEN, link_last_resp_len);
        link_last_resp_valid = 1;
        link_rx_ack = seq;
        break;
    case LINK_OP_REQUEST:
    case LINK_OP_DATA:
    case LINK_OP_EOF:
    case LINK_OP_ACK:
    case LINK_OP_WINDOW:
    default:
        /* L0 では OS32 が要求側。Host からの REQUEST/DATA は L2/L3 で扱う。 */
        break;
    }
}

void link_poll(void)
{
    unsigned int len = 0;
    int rc;
    if (!link_ready) return;
    ne2k_poll(LINK_POLL_BUDGET);
    for (;;) {
        rc = ne2k_recv(link_rxbuf, sizeof(link_rxbuf), &len);
        if (rc != NE2K_OK) break;
        link_dispatch(link_rxbuf, len);
    }
}

/* 期限まで受信を回す。deadline は tick_count 基準。 */
static void link_pump_until(u32 deadline)
{
    do {
        link_poll();
    } while ((i32)(deadline - tick_count) > 0 && !link_last_resp_valid);
}

int link_hello(int tries)
{
    int t;
    if (!link_ready) return LINK_ERR;
    for (t = 0; t < tries; t++) {
        u32 deadline;
        if (link_send(LINK_OP_HELLO, link_bcast, 0, link_rx_ack, link_my_mac, 6) != LINK_OK)
            return LINK_ERR;
        deadline = tick_count + LINK_RTO_TICKS;
        while ((i32)(deadline - tick_count) > 0) {
            link_poll();
            if (link_hello_ok) return LINK_OK;
        }
        if (t + 1 < tries) link_retransmits++;
    }
    return LINK_ERR_TIMEOUT;
}

int link_request(const void *req, unsigned int req_len,
                 void *resp, unsigned int resp_cap, unsigned int *resp_len)
{
    int t;
    u32 seq;
    if (!link_ready) return LINK_ERR;
    if (!link_hello_ok) return LINK_ERR_NOPEER;
    if (req_len > LINK_MAX_PAYLOAD) return LINK_ERR_TOOBIG;

    seq = link_tx_seq;
    for (t = 0; t < 3; t++) {
        u32 deadline;
        link_last_resp_valid = 0;
        if (link_send(LINK_OP_REQUEST, link_peer_mac, seq, link_rx_ack, req, req_len) != LINK_OK)
            return LINK_ERR;
        deadline = tick_count + LINK_RTO_TICKS;
        link_pump_until(deadline);
        /* この seq に対する RESPONSE が来たか (ack が一致、または応答 seq を確認) */
        if (link_last_resp_valid && link_last_resp_ack == seq) {
            unsigned int n = link_last_resp_len;
            if (n > resp_cap) n = resp_cap;
            if (resp && n) kmemcpy(resp, link_last_resp, n);
            if (resp_len) *resp_len = link_last_resp_len;
            /* ACK を返してから次の seq へ (ACK まで次を出さない) */
            link_send(LINK_OP_ACK, link_peer_mac, seq, seq, 0, 0);
            link_tx_seq = seq + 1;
            link_rt_ok++;
            return LINK_OK;
        }
        link_retransmits++;
    }
    link_rt_fail++;
    return LINK_ERR_TIMEOUT;
}

void link_selftest(int rounds)
{
    int i;
    char ping[LINK_PING_LEN];
    u8 resp[LINK_MAX_PAYLOAD];
    unsigned int rlen;

    if (link_hello(5) != LINK_OK) {
        kprintf(0xC1, "[link] HELLO failed (no Host Agent?) — L0 selftest skipped\n");
        return;
    }
    kprintf(0x07, "[link] HELLO ok, peer %02x:%02x:%02x:%02x:%02x:%02x epoch %d\n",
            link_peer_mac[0], link_peer_mac[1], link_peer_mac[2],
            link_peer_mac[3], link_peer_mac[4], link_peer_mac[5], (int)link_epoch);

    for (i = 0; i < rounds; i++) {
        /* "PING nnnn" を埋める (固定長) */
        kmemset(ping, 0, sizeof(ping));
        ping[0] = 'P'; ping[1] = 'I'; ping[2] = 'N'; ping[3] = 'G'; ping[4] = ' ';
        ping[5] = (char)('0' + (i / 1000) % 10);
        ping[6] = (char)('0' + (i / 100) % 10);
        ping[7] = (char)('0' + (i / 10) % 10);
        ping[8] = (char)('0' + i % 10);
        rlen = 0;
        (void)link_request(ping, LINK_PING_LEN, resp, sizeof(resp), &rlen);
    }
    kprintf(link_rt_fail ? 0xC1 : 0x07,
            "[link] L0 selftest: %d/%d round trips ok, %d retransmit\n",
            (int)link_rt_ok, rounds, (int)link_retransmits);
}
