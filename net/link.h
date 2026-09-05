/* ======================================================================== */
/*  LINK.H — OS32 リンクプロトコル (LGY-98 の上の独自 raw Ethernet)          */
/*                                                                          */
/*  計画: docs/tasks/network/LINK_PLAN.md。ドライバ (drivers/ne2000.c) の上に  */
/*  載る「確実に届ける・溢れさせない」層。TCP/IP は載せず、Host Agent との    */
/*  P2P で REQUEST/RESPONSE と DATA ストリームをやり取りする。               */
/*                                                                          */
/*  この段階は L0 (Stop-and-Wait): HELLO 交換 → 1 要求 1 応答の往復 → ACK。  */
/*  WINDOW/Credit (L1) と DATA ストリーミング (L2) は後続。                  */
/* ======================================================================== */

#ifndef OS32_NET_LINK_H
#define OS32_NET_LINK_H

#include "types.h"

/* 独自 EtherType (IEEE 802.1 local experimental 1)。Host Agent と共通。 */
#define LINK_ETHERTYPE      0x88B5
#define LINK_ETH_ADDR_LEN   6
#define LINK_ETH_HDR_LEN    14      /* dst(6) + src(6) + type(2) */

/* opcode (link_hdr.op) */
#define LINK_OP_HELLO       1       /* MAC / epoch / 初期 window 交換、再同期 */
#define LINK_OP_REQUEST     2       /* OS32 → Host: 要求 (request_id) */
#define LINK_OP_RESPONSE    3       /* Host → OS32: 要求の結果 */
#define LINK_OP_DATA        4       /* ストリームのペイロード (L2) */
#define LINK_OP_EOF         5       /* ストリーム終端 (L2) */
#define LINK_OP_ACK         6       /* ack_seq まで受信・処理済み */
#define LINK_OP_WINDOW      7       /* 絶対値 credit の広告 (L1) */

/* リンクヘッダ (Ethernet ペイロード先頭)。x86 なのでそのまま little-endian で
 * 送受信し、Host Agent 側も LE で読む。16 バイト。 */
struct link_hdr {
    u8  op;
    u8  flags;
    u16 epoch;
    u32 seq;        /* 送信側のフレーム連番 */
    u32 ack;        /* 受信済みの最終 in-order seq (累積 ACK) */
    u16 length;     /* このヘッダに続くペイロードのバイト数 */
    u16 rsvd;
};

#define LINK_HDR_LEN        16
#define LINK_MAX_PAYLOAD    (1514 - LINK_ETH_HDR_LEN - LINK_HDR_LEN)  /* 1484 */

/* 結果コード */
#define LINK_OK             0
#define LINK_ERR            -1      /* ドライバ / 送信エラー */
#define LINK_ERR_TIMEOUT    -2      /* 応答が来ない (再送しても) */
#define LINK_ERR_NOPEER     -3      /* HELLO 未完了 (peer MAC 不明) */
#define LINK_ERR_TOOBIG     -4      /* ペイロードが LINK_MAX_PAYLOAD 超過 */

/* リンク層を初期化する (ドライバ attach 後に呼ぶ)。my_mac は NIC の MAC。 */
void link_init(const u8 my_mac[6]);

/* HELLO を送って Host Agent と peer MAC / epoch を確立する。
 * 応答が来るまで tries 回まで再送する。成功で LINK_OK。 */
int  link_hello(int tries);

/* Stop-and-Wait の 1 往復: REQUEST を送り、対応する RESPONSE を待つ。
 * 応答本文を resp に最大 resp_cap バイトコピーし、*resp_len に実長を入れる。
 * 応答が来るまで tries 回まで再送する。ACK が返るまで次を送らない設計なので、
 * この関数が戻るまで呼び出し側は次の要求を出さない。 */
int  link_request(const void *req, unsigned int req_len,
                  void *resp, unsigned int resp_cap, unsigned int *resp_len);

/* 受信フレームを 1 つ処理する (L0 では link_request 内で使う)。呼び出し側の
 * ループから使えるよう公開。データが無ければ何もしない。 */
void link_poll(void);

/* L0 自己試験 (LGY98_FLAG_LINKTEST): HELLO のあと rounds 回 PING/PONG を往復し、
 * 成否をログと下のカウンタに残す。Host Agent が起動している前提。 */
void link_selftest(int rounds);

/* L1 自己試験: ホストに DATA を count フレーム (各 payload バイト) 流させ、
 * 絶対値 WINDOW でフロー制御する。溢れさせずに全フレームを順序どおり受ける
 * ことと、ページ消費の実測 (§2-3) を確認する。link_hello 済みが前提。 */
void link_l1_bulk(unsigned int count, unsigned int payload);

/* ホストから kernel.map 経由で観測するカウンタ (static にしない)。 */
extern u32 link_hello_ok;       /* HELLO 確立 (0/1) */
extern u32 link_rt_ok;          /* 成功した往復数 */
extern u32 link_rt_fail;        /* 応答が来なかった往復数 */
extern u32 link_retransmits;    /* 再送回数 */
extern u32 link_rx_frames;      /* 受け取ったリンクフレーム数 */
extern u32 link_rx_dropped;     /* 自分宛でない / 壊れた等で捨てた数 */
extern u8  link_peer_mac[6];    /* 確立した Host Agent の MAC */
extern u16 link_epoch;

/* L1 (WINDOW/Credit の bulk 受信) 観測用 */
extern u32 link_l1_recv;        /* 順序どおり受けた DATA フレーム数 */
extern u32 link_l1_bytes;       /* 受けた DATA の総ペイロードバイト数 */
extern u32 link_l1_ooo;         /* 順序外 / 欠落 (期待 seq と不一致) の回数 */
extern u32 link_l1_windows;     /* 送った WINDOW 数 */
extern u32 link_l1_max_credit;  /* 広告した credit_pages の最大値 */
extern u32 link_l1_min_credit;  /* 広告した credit_pages の最小値 (0 除く) */
extern u32 link_l1_done;        /* EOF を受けた (0/1) */
extern u32 link_l1_meas_pages;  /* 実測: DATA 1 フレームあたりの平均消費ページ ×100 */

#endif /* OS32_NET_LINK_H */
