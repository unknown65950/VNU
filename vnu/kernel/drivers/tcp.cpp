#include <vnu/tcp.h>
#include <vnu/abi.h>
#include <stdint.h>

#include "netcore.h"


/* TCP/IPv4 stack for the hobby kernel: one client connection per slot,
 * plus bound/listen/accept server sockets sharing the same fixed table.
 *
 * Model: one port/one connection per slot, blocking ops driven from the
 * syscall (busy-wait + rx_drain, like ping/DNS). Sends are window-limited
 * with timeout-based retransmission of the oldest unacked bytes; receives
 * accept only in-order data (out-of-order / duplicate segments are dropped,
 * the running ACK is re-sent).
 *
 * Server side: a socket in ST_LISTEN answers inbound SYNs by spawning a
 * child socket in ST_SYN_RCVD (backlog-bounded), completes the handshake
 * on the final ACK and parks the child in the listener's accept queue
 * until accept() hands it to userspace. Children are looked up by the
 * full four-tuple; the listener by its local port alone.
 *
 * 127.0.0.0/8 is a software loopback: connect() skips ARP, and every
 * segment to a loopback address is re-fed into the input path from
 * tcp_send_seg (synchronous, no NIC) — so a guest process can open a
 * TCP connection to a server inside the same guest.
 *
 * The FIN occupies its own sequence number and is retransmitted until
 * acked; only after all buffered data is pushed is the FIN emitted. */

extern "C" void* memcpy(void* dst, const void* src, unsigned long count);
extern "C" void* memmove(void* dst, const void* src, unsigned long count);
extern "C" int memcmp(const void* a, const void* b, unsigned long count);
extern "C" void vnu_debug_putc(char c);


namespace {

constexpr int MAX_SOCKS = 8;   /* fixed socket table */
constexpr int SEND_CAP = 16384; /* per-socket send buffer */
constexpr int RECV_CAP = 16384; /* per-socket receive buffer */
constexpr int PUSH_MSS = 1460;  /* we advertise (and usually send) 1460 */
constexpr int DEFAULT_MSS = 536; /* RFC 793 default for peers without MSS */
constexpr int MAX_RTO = 8000;   /* retransmission backoff ceiling, ms */
constexpr int INITIAL_RTO = 1000;
constexpr int MAX_ACCEPT = 4;   /* listen() backlog / accept queue cap */

enum State {
    ST_CLOSED,
    ST_LISTEN,
    ST_SYN_RCVD,
    ST_SYN_SENT,
    ST_ESTABLISHED,
    ST_FIN_WAIT1,
    ST_FIN_WAIT2,
    ST_CLOSE_WAIT,
};

enum {
    FLAG_FIN = 0x01,
    FLAG_SYN = 0x02,
    FLAG_RST = 0x04,
    FLAG_PSH = 0x08,
    FLAG_ACK = 0x10,
};

struct TcpSock {
    bool used = false;
    int state = ST_CLOSED;

    uint32_t rip = 0;    /* remote address, big-endian */
    uint16_t rport = 0;  /* remote port, host order */
    uint16_t lport = 0;  /* our port, host order */
    uint32_t iss = 0;    /* our initial sequence number */
    uint8_t dst_mac[6];  /* resolved gateway/peer MAC */

    /* send side */
    uint8_t  sndbuf[SEND_CAP];
    uint32_t snd_una = 0;  /* seq of the first (oldest) buffered byte */
    uint32_t snd_nxt = 0;  /* seq of the next byte to transmit */
    uint32_t snd_used = 0; /* bytes buffered from snd_una */
    uint16_t peer_mss = DEFAULT_MSS;
    uint32_t snd_wnd = 0;  /* remote advertised window */
    uint32_t fin_seq = 0;  /* seq of our FIN, once sent */
    bool fin_sent = false;
    bool fin_acked = false;
    uint32_t rto = INITIAL_RTO;
    uint32_t rto_at = 0;   /* when the oldest unacked byte was last sent */
    bool rto_active = false;

    /* receive side */
    uint8_t  rcvbuf[RECV_CAP];
    uint32_t rcv_base = 0; /* seq of rcvbuf[0] */
    uint32_t rcv_used = 0; /* in-order bytes buffered */

    bool fin_rcvd = false;
    bool err_reset = false;

    /* server side */
    int listener = -1;       /* LISTEN socket that spawned us, -1 otherwise */
    int8_t accept_q[MAX_ACCEPT]; /* completed handshakes awaiting accept() */
    uint8_t accept_q_len = 0;
    uint8_t accept_q_head = 0;
    int backlog = 0;         /* listen() backlog, clamped to 1..MAX_ACCEPT */
    bool queued = false;     /* parked in the listener's accept queue */
};

TcpSock g_socks[MAX_SOCKS];

void put_be16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint16_t get_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t checksum_add(uint32_t sum, const uint8_t* d, unsigned len)
{
    unsigned i = 0;
    for (; i + 1 < len; i += 2)
        sum += (static_cast<uint32_t>(d[i]) << 8) | d[i + 1];
    if (i < len)
        sum += static_cast<uint32_t>(d[i]) << 8;
    return sum;
}

static uint16_t checksum_done(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

bool valid_handle(int sock, TcpSock** out)
{
    if (sock < 0 || sock >= MAX_SOCKS || !g_socks[sock].used)
        return false;
    *out = &g_socks[sock];
    return true;
}

/* Sequence number of the next byte the peer must send to us. Includes the
 * +1 for the peer's FIN once it has been received. */
uint32_t rcv_ackno(const TcpSock& s)
{
    return s.rcv_base + s.rcv_used + (s.fin_rcvd ? 1u : 0u);
}

uint16_t rcv_window(const TcpSock& s)
{
    uint32_t free = RECV_CAP - s.rcv_used;
    return free > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(free);
}

/* Builds and transmits one TCP segment in the driver's TX buffer. `pay`
 * and `paylen` are the payload (may be null/0); `syn_opt` adds our MSS
 * option on a SYN. Computes both the IP and TCP checksums in place. */
void tcp_send_seg(TcpSock& s, uint32_t seq, uint32_t ackno, uint8_t flags,
                  bool syn_opt, const uint8_t* pay, uint16_t paylen)
{
    const uint16_t tcp_hdr = static_cast<uint16_t>(20u + (syn_opt ? 4u : 0u));
    const uint16_t frame_len = static_cast<uint16_t>(14 + 20 + tcp_hdr + paylen);
    /* 127.0.0.0/8 goes to our own loopback: src becomes 127.0.0.1 and
     * the frame is fed back into the input path instead of the NIC. */
    const uint32_t src_ip = (s.rip & 0xFF000000u) == 0x7F000000u
                                ? 0x7F000001u
                                : vnu::netcore::our_ip();

    vnu::netcore::eth_header(0x0800, s.dst_mac);
    uint8_t* p = vnu::netcore::tx_room();

    p[0] = 0x45;
    p[1] = 0;
    put_be16(p + 2, static_cast<uint16_t>(20 + tcp_hdr + paylen));
    put_be16(p + 4, static_cast<uint16_t>(seq & 0xFFFFu)); /* IP id */
    put_be16(p + 6, 0); /* flags/fragment offset */
    p[8] = 64;          /* TTL */
    p[9] = 6;           /* TCP */
    put_be16(p + 10, 0); /* IP checksum, filled below */
    put_be32(p + 12, src_ip);
    put_be32(p + 16, s.rip);

    uint8_t* t = p + 20;
    put_be16(t + 0, s.lport);
    put_be16(t + 2, s.rport);
    put_be32(t + 4, seq);
    put_be32(t + 8, ackno);
    t[12] = static_cast<uint8_t>((tcp_hdr / 4u) << 4);
    t[13] = flags;
    put_be16(t + 14, rcv_window(s));
    put_be16(t + 16, 0); /* TCP checksum, filled below */
    put_be16(t + 18, 0); /* urgent pointer */

    uint16_t n = 20;
    if (syn_opt) {
        t[n++] = 2; /* MSS */
        t[n++] = 4;
        put_be16(t + n, PUSH_MSS);
        n += 2;
    }
    if (paylen && pay)
        for (uint16_t i = 0; i < paylen; ++i)
            t[n++] = pay[i];
    (void)n;

    uint8_t pseudo[12];
    put_be32(pseudo + 0, src_ip);
    put_be32(pseudo + 4, s.rip);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put_be16(pseudo + 10, static_cast<uint16_t>(tcp_hdr + paylen));
    uint32_t sum = checksum_add(0, pseudo, 12);
    sum = checksum_add(sum, t, tcp_hdr + paylen);
    put_be16(t + 16, checksum_done(sum));

    put_be16(p + 10, checksum_done(checksum_add(0, p, 20)));
    if ((s.rip & 0xFF000000u) == 0x7F000000u) {
        /* Loopback: no NIC involved — hand the fully-assembled frame
         * straight back to the input path (synchronous, like a lo0
         * interface that loops the packet). The TCP input path does
         * not check the destination address, matching how the driver
         * already dispatches NIC frames to it. */
        vnu::tcp::input_packet(s.rip, p - 14, frame_len,
                               static_cast<uint16_t>(20 + tcp_hdr + paylen));
        return;
    }
    vnu::netcore::tx_send_current(frame_len);
}

/* Pushes buffered unsent bytes onto the wire, up to the peer window. */
void tcp_push(TcpSock& s)
{
    uint32_t inflight = s.snd_nxt - s.snd_una;
    uint32_t unsent = s.snd_used - inflight;
    uint32_t window = s.snd_wnd;

    while (unsent && inflight < window) {
        uint16_t seg = PUSH_MSS;
        uint16_t allow = static_cast<uint16_t>(window - inflight);
        if (seg > allow)
            seg = allow;
        if (seg > s.peer_mss)
            seg = s.peer_mss;
        if (seg > unsent)
            seg = static_cast<uint16_t>(unsent);
        if (seg == 0)
            break;

        uint32_t off = s.snd_nxt - s.snd_una;
        tcp_send_seg(s, s.snd_nxt, rcv_ackno(s), FLAG_ACK, false,
                     s.sndbuf + off, seg);

        if (!s.rto_active) {
            s.rto_active = true;
            s.rto = INITIAL_RTO;
            s.rto_at = vnu::netcore::now_ms();
        }
        s.snd_nxt += seg;
        inflight = s.snd_nxt - s.snd_una;
        unsent = s.snd_used - inflight;
    }
}

/* Standalone ACK (also doubles as a window update). */
void tcp_send_ack(TcpSock& s)
{
    tcp_send_seg(s, s.snd_nxt, rcv_ackno(s), FLAG_ACK, false, nullptr, 0);
}

/* Sends our FIN (sequence number reservation only). */
void tcp_send_fin(TcpSock& s)
{
    s.fin_seq = s.snd_nxt;
    tcp_send_seg(s, s.snd_nxt, rcv_ackno(s), FLAG_ACK | FLAG_FIN, false,
                 nullptr, 0);
    s.snd_nxt += 1;
    s.rto_active = true;
    s.rto_at = vnu::netcore::now_ms();
}

/* Timeout-based retransmission. Resends the SYN while connecting, the FIN
 * when it is the only outstanding thing, otherwise the oldest unacked
 * window of data (rewind snd_nxt and repush). */
void tcp_retransmit(TcpSock& s, uint32_t now)
{
    if (!s.rto_active)
        return;
    if (static_cast<uint32_t>(now - s.rto_at) < s.rto)
        return;
    s.rto_at = now;
    if (s.rto < MAX_RTO)
        s.rto *= 2;

    if (s.state == ST_SYN_SENT) {
        tcp_send_seg(s, s.iss, 0, FLAG_SYN, true, nullptr, 0);
        return;
    }
    if (s.state == ST_SYN_RCVD) {
        /* Server side: refresh the SYN-ACK until the peer's ACK lands. */
        tcp_send_seg(s, s.iss, s.rcv_base, FLAG_SYN | FLAG_ACK, true,
                     nullptr, 0);
        return;
    }
    if (s.fin_sent && !s.fin_acked && s.snd_used == 0) {
        tcp_send_fin(s);
        return;
    }
    s.snd_nxt = s.snd_una;
    tcp_push(s);
}

/* Handles a cumulative ACK. Advances the send window over acknowledged
 * data and/or our FIN; frees buffer space and kicks the push loop. */
void acknowledge(TcpSock& s, uint32_t ackno, uint16_t window)
{
    if (ackno <= s.snd_una)
        return;
    uint32_t data_end = s.snd_una + s.snd_used;
    if (ackno > data_end + (s.fin_sent ? 1u : 0u))
        return; /* ack for bytes we never sent (bogus) */

    if (s.fin_sent && !s.fin_acked && ackno > s.fin_seq)
        s.fin_acked = true;

    uint32_t adv = ackno - s.snd_una;
    if (adv > s.snd_used)
        adv = s.snd_used;
    if (adv) {
        memmove(s.sndbuf, s.sndbuf + adv, s.snd_used - adv);
        s.snd_used -= adv;
        s.snd_una += adv;
    }
    s.snd_wnd = window;
    s.rto = INITIAL_RTO;
    s.rto_at = vnu::netcore::now_ms();
    if (s.snd_used == 0 && (!s.fin_sent || s.fin_acked))
        s.rto_active = false;
    tcp_push(s);
}

/* Matches a socket by the full four-tuple: peers (client or server
 * child) are found by src IP, src port and dst port. The LISTEN socket
 * itself (rip == 0, rport == 0) never matches here — it is found by
 * local port only, in find_listener(). */
int find_peer_sock(uint32_t src_ip, uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < MAX_SOCKS; ++i) {
        TcpSock& s = g_socks[i];
        if (s.used && s.rip == src_ip && s.rport == sport && s.lport == dport)
            return i;
    }
    return -1;
}

int find_listener(uint16_t dport)
{
    for (int i = 0; i < MAX_SOCKS; ++i) {
        TcpSock& s = g_socks[i];
        if (s.used && s.state == ST_LISTEN && s.lport == dport)
            return i;
    }
    return -1;
}

/* Cheap per-connection ISN: a mix of the clock and both ends' addresses,
 * so simultaneous connections in adjacent slots diverge. */
uint32_t derive_iss(uint32_t now, uint32_t rip, uint16_t rport, uint16_t lport)
{
    uint32_t iss = (now << 8) ^ (rip >> 16) ^ (static_cast<uint32_t>(rport) << 16) ^
                   (static_cast<uint32_t>(lport) << 8);
    return iss ? iss : 0x12345678u;
}

/* --- listener accept queue ------------------------------------------ */

/* Parking a child in its listener's queue. The backlog cap makes sure
 * this can never overflow: pending (SYN_RCVD + queued) children of a
 * listener stay <= backlog <= MAX_ACCEPT from the moment of the SYN. */
void accept_push(TcpSock& lst, TcpSock& child)
{
    auto pos = static_cast<uint8_t>((lst.accept_q_head + lst.accept_q_len) % MAX_ACCEPT);
    lst.accept_q[pos] = static_cast<int8_t>(&child - g_socks);
    ++lst.accept_q_len;
    child.queued = true;
}

/* Removes a specific child from its listener's queue (used when the
 * child is closed before being accepted). */
void accept_remove(TcpSock& lst, int child_slot)
{
    for (uint8_t k = 0; k < lst.accept_q_len; ++k) {
        uint8_t pos = static_cast<uint8_t>((lst.accept_q_head + k) % MAX_ACCEPT);
        if (lst.accept_q[pos] == child_slot) {
            for (uint8_t m = static_cast<uint8_t>(k + 1); m < lst.accept_q_len; ++m)
                lst.accept_q[static_cast<uint8_t>((lst.accept_q_head + m - 1) % MAX_ACCEPT)] =
                    lst.accept_q[static_cast<uint8_t>((lst.accept_q_head + m) % MAX_ACCEPT)];
            --lst.accept_q_len;
            return;
        }
    }
}

/* Handshake + queued connections a listener still owns. Bounds the
 * socket-table pressure of a server: never more than its backlog. */
int listener_pending(int lst_slot)
{
    int n = 0;
    for (int i = 0; i < MAX_SOCKS; ++i) {
        const TcpSock& s = g_socks[i];
        if (s.used && s.listener == lst_slot &&
            (s.state == ST_SYN_RCVD || s.queued))
            ++n;
    }
    return n;
}

/* Parses the MSS option out of a SYN/SYN-ACK option block. */
uint16_t parse_mss_opt(const uint8_t* t, int hdr_len)
{
    int opt = 20;
    while (opt + 1 < hdr_len) {
        uint8_t kind = t[opt];
        if (kind == 0)
            break;
        if (kind == 1) {
            ++opt;
            continue;
        }
        uint8_t olen = t[opt + 1];
        if (olen < 2 || opt + olen > hdr_len)
            break;
        if (kind == 2 && olen == 4) {
            uint16_t mss = get_be16(t + opt + 2);
            if (mss > 0)
                return mss;
        }
        opt += olen;
    }
    return 0;
}

/* Completes the handshake: SYN-ACK with the right ack number. */
void handle_syn_ack(TcpSock& s, uint32_t seq, uint32_t ackno,
                    const uint8_t* tcp_hdr_raw, int tcp_hdr_len, uint16_t window)
{
    if (ackno != s.iss + 1u)
        return;
    s.peer_mss = parse_mss_opt(tcp_hdr_raw, tcp_hdr_len);
    if (s.peer_mss == 0)
        s.peer_mss = DEFAULT_MSS;
    s.snd_wnd = window;
    s.snd_una = s.iss + 1u;
    s.snd_nxt = s.iss + 1u;
    s.rcv_base = seq + 1u; /* peer's ISN */
    s.rcv_used = 0;
    s.rto_active = false;
    s.state = ST_ESTABLISHED;
    tcp_send_ack(s);
}

/* Server side of the handshake: an inbound SYN for a LISTEN socket.
 * Spawns a child socket in ST_SYN_RCVD, seeds its sequence windows from
 * the SYN and answers with a SYN-ACK. `src_mac` is the peer's MAC from
 * the Ethernet frame, used directly as the child's destination. */
void handle_incoming_syn(uint32_t src_ip, const uint8_t* src_mac,
                         uint16_t sport, uint16_t dport, uint32_t seq,
                         uint16_t window, const uint8_t* tcp_hdr_raw,
                         int tcp_hdr_len)
{
    /* A repeated SYN for a child that already exists just re-arms the
     * SYN-ACK (the peer never saw the first one). */
    int exists = find_peer_sock(src_ip, sport, dport);
    if (exists >= 0 && g_socks[exists].state == ST_SYN_RCVD) {
        TcpSock& c = g_socks[exists];
        tcp_send_seg(c, c.iss, c.rcv_base, FLAG_SYN | FLAG_ACK, true,
                     nullptr, 0);
        return;
    }

    int lst = find_listener(dport);
    if (lst < 0)
        return; /* nothing listening: drop, like other unknown 4-tuples */
    if (listener_pending(lst) >= g_socks[lst].backlog)
        return; /* backlog full: drop so the peer's retries land later */

    int slot = -1;
    for (int i = 0; i < MAX_SOCKS; ++i)
        if (!g_socks[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return; /* no table room: drop the SYN */

    TcpSock& c = g_socks[slot];
    c = TcpSock{}; /* zero-initialize: fresh ring/queue state */
    c.used = true;
    c.rip = src_ip;
    c.rport = sport;
    c.lport = dport;
    c.listener = lst;
    c.state = ST_SYN_RCVD;

    uint32_t now = vnu::netcore::now_ms();
    c.iss = derive_iss(now, src_ip, sport, dport);
    c.snd_una = c.iss;
    c.snd_nxt = c.iss;
    c.rcv_base = seq + 1u; /* the SYN consumed the peer's initial seq */
    c.rcv_used = 0;
    c.peer_mss = parse_mss_opt(tcp_hdr_raw, tcp_hdr_len);
    if (c.peer_mss == 0)
        c.peer_mss = DEFAULT_MSS;
    c.snd_wnd = window;
    c.rto = INITIAL_RTO;
    c.rto_at = now;
    c.rto_active = true;
    for (int i = 0; i < 6; ++i)
        c.dst_mac[i] = src_mac[i];

    tcp_send_seg(c, c.iss, c.rcv_base, FLAG_SYN | FLAG_ACK, true, nullptr, 0);
}

} // namespace

namespace vnu::tcp {

void input_packet(uint32_t src_ip, const uint8_t* f, uint16_t frame_len,
                  uint16_t ip_len)
{
    (void)frame_len;
    if (ip_len < 20)
        return;
    uint8_t doff = static_cast<uint8_t>(f[46] >> 4);
    int tcp_hdr_len = doff * 4;
    if (tcp_hdr_len < 20)
        return;

    uint16_t sport = get_be16(f + 34);
    uint16_t dport = get_be16(f + 36);
    uint32_t seq = (static_cast<uint32_t>(f[38]) << 24) | (static_cast<uint32_t>(f[39]) << 16) |
                   (static_cast<uint32_t>(f[40]) << 8) | f[41];
    uint32_t ackno = (static_cast<uint32_t>(f[42]) << 24) | (static_cast<uint32_t>(f[43]) << 16) |
                     (static_cast<uint32_t>(f[44]) << 8) | f[45];
    uint8_t flags = f[47];
    uint16_t window = get_be16(f + 48);

    const uint8_t* pay = f + 34 + tcp_hdr_len;
    uint32_t paylen = static_cast<uint32_t>(ip_len) - 20u - static_cast<uint32_t>(tcp_hdr_len);

    /* SYN goes to the listener path, not to an existing socket. */
    if (flags & FLAG_SYN) {
        handle_incoming_syn(src_ip, f + 6, sport, dport, seq, window,
                            f + 34, tcp_hdr_len);
        return;
    }

    int sock = find_peer_sock(src_ip, sport, dport);
    if (sock < 0)
        return; /* no socket for this four-tuple: drop */

    TcpSock& s = g_socks[sock];

    if (flags & FLAG_RST) {
        s.err_reset = true;
        return;
    }

    if (s.state == ST_SYN_SENT) {
        if ((flags & (FLAG_SYN | FLAG_ACK)) == (FLAG_SYN | FLAG_ACK))
            handle_syn_ack(s, seq, ackno, f + 34, tcp_hdr_len, window);
        return;
    }

    if (s.state == ST_SYN_RCVD) {
        /* The final ACK of the server handshake. The SYN is our only
         * outstanding byte, so the peer must be acking iss+1; rejecting
         * anything else matches acknowledge()'s "bogus ack" rule. */
        if (!(flags & FLAG_ACK))
            return;
        if (ackno != s.iss + 1u)
            return;
        s.snd_una = s.iss + 1u;
        s.snd_nxt = s.iss + 1u;
        s.snd_wnd = window;
        s.rto_active = false;
        s.state = ST_ESTABLISHED;
        if (s.listener >= 0)
            accept_push(g_socks[s.listener], s);
        /* fall through: the same segment may carry data or a FIN */
    }

    if (s.state != ST_ESTABLISHED && s.state != ST_CLOSE_WAIT &&
        s.state != ST_FIN_WAIT1 && s.state != ST_FIN_WAIT2)
        return;

    if (flags & FLAG_ACK)
        acknowledge(s, ackno, window);

    if (s.state == ST_FIN_WAIT1 && s.fin_acked)
        s.state = ST_FIN_WAIT2;

    bool need_ack = false;

    if (paylen && s.rcv_used + paylen <= RECV_CAP) {
        uint32_t expected = s.rcv_base + s.rcv_used;
        if (seq == expected) {
            for (uint32_t i = 0; i < paylen; ++i)
                s.rcvbuf[s.rcv_used + i] = pay[i];
            s.rcv_used += paylen;
            need_ack = true;
        } else if (seq < expected) {
            tcp_send_ack(s); /* duplicate/retransmitted: re-ack */
            return;
        } else {
            return; /* out-of-order future segment: drop, keep ack as-is */
        }
    }

    if (flags & FLAG_FIN) {
        if (!s.fin_rcvd) {
            s.fin_rcvd = true;
            if (s.state == ST_ESTABLISHED)
                s.state = ST_CLOSE_WAIT;
            else if (s.state == ST_FIN_WAIT2)
                s.state = ST_CLOSED;
            need_ack = true;
        }
    }

    if (need_ack)
        tcp_send_ack(s);
}

int socket_open(int family, int type)
{
    if (family != 2 || type != 1) /* AF_INET only, SOCK_STREAM only */
        return -VNU_EINVAL;
    for (int i = 0; i < MAX_SOCKS; ++i) {
        if (!g_socks[i].used) {
            g_socks[i] = TcpSock{}; /* zero-initialize */
            g_socks[i].used = true;
            g_socks[i].lport = static_cast<uint16_t>(49152u + 7u * i);
            return i;
        }
    }
    return -VNU_EMFILE;
}

long socket_connect(int sock, uint32_t ip_be, uint16_t port, uint32_t timeout_ms)
{
    TcpSock* sp = nullptr;
    if (!valid_handle(sock, &sp))
        return -VNU_EBADF;
    TcpSock& s = *sp;

    if (timeout_ms < 20)
        timeout_ms = 20;
    if (timeout_ms > 20000)
        timeout_ms = 20000;

    s.rip = ip_be;
    s.rport = port;
    uint32_t now = vnu::netcore::now_ms();
    s.iss = derive_iss(now, ip_be, port, s.lport);
    s.snd_una = s.iss;
    s.snd_nxt = s.iss;
    s.snd_used = 0;
    s.peer_mss = DEFAULT_MSS;
    s.snd_wnd = 0;
    s.rto = INITIAL_RTO;
    s.rto_active = true;
    s.rto_at = now;
    s.state = ST_SYN_SENT;

    int mac_rc = 0;
    if ((ip_be & 0xFF000000u) == 0x7F000000u) {
        /* Loopback: the peer is ourselves; the MAC is irrelevant and
         * nothing answers ARP for 127.0.0.1 on the wire. */
        for (int i = 0; i < 6; ++i)
            s.dst_mac[i] = 0;
    } else {
        mac_rc = vnu::netcore::arp_ensure(ip_be, s.dst_mac, timeout_ms);
        if (mac_rc < 0)
            return mac_rc;
    }

    tcp_send_seg(s, s.iss, 0, FLAG_SYN, true, nullptr, 0);

    uint32_t deadline = vnu::netcore::now_ms() + timeout_ms;
    while (static_cast<int32_t>(vnu::netcore::now_ms() - deadline) < 0) {
        vnu::netcore::rx_drain();
        if (s.state == ST_ESTABLISHED)
            return 0;
        if (s.err_reset)
            return -VNU_ECONNREFUSED;
        tcp_retransmit(s, vnu::netcore::now_ms());
    }
    return -VNU_ETIMEDOUT;
}

long socket_send(int sock, const void* buf, uint32_t len, uint32_t timeout_ms)
{
    TcpSock* sp = nullptr;
    if (!valid_handle(sock, &sp))
        return -VNU_EBADF;
    TcpSock& s = *sp;
    if (s.state != ST_ESTABLISHED && s.state != ST_CLOSE_WAIT)
        return -VNU_ENOTCONN;
    if (!buf || len == 0)
        return 0;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(buf);
    uint32_t todo = len;
    uint32_t deadline = vnu::netcore::now_ms() + timeout_ms;

    while (todo) {
        while (s.snd_used >= SEND_CAP) {
            vnu::netcore::rx_drain();
            if (s.err_reset)
                return -VNU_ECONNRESET;
            if (static_cast<int32_t>(vnu::netcore::now_ms() - deadline) >= 0)
                return -VNU_ETIMEDOUT;
        }
        uint32_t chunk = todo;
        if (chunk > SEND_CAP - s.snd_used)
            chunk = SEND_CAP - s.snd_used;
        memcpy(s.sndbuf + s.snd_used, data, chunk);
        s.snd_used += chunk;
        data += chunk;
        todo -= chunk;
        tcp_push(s);

        if (s.err_reset)
            return -VNU_ECONNRESET;
        if (todo && static_cast<int32_t>(vnu::netcore::now_ms() - deadline) >= 0)
            return -VNU_ETIMEDOUT;
    }
    return static_cast<long>(len);
}

long socket_recv(int sock, void* buf, uint32_t len, uint32_t timeout_ms)
{
    TcpSock* sp = nullptr;
    if (!valid_handle(sock, &sp))
        return -VNU_EBADF;
    TcpSock& s = *sp;
    if (s.state == ST_CLOSED && !s.fin_rcvd)
        return -VNU_ENOTCONN;
    if (!buf || len == 0)
        return 0;

    uint8_t* out = reinterpret_cast<uint8_t*>(buf);
    uint32_t deadline = vnu::netcore::now_ms() + timeout_ms;

    for (;;) {
        vnu::netcore::rx_drain();
        if (s.err_reset)
            return -VNU_ECONNRESET;

        if (s.rcv_used) {
            uint32_t take = len;
            if (take > s.rcv_used)
                take = s.rcv_used;
            for (uint32_t i = 0; i < take; ++i)
                out[i] = s.rcvbuf[i];
            memmove(s.rcvbuf, s.rcvbuf + take, s.rcv_used - take);
            s.rcv_used -= take;
            s.rcv_base += take;
            tcp_send_ack(s); /* window update so the peer resumes sending */
            return static_cast<long>(take);
        }
        if (s.fin_rcvd)
            return 0;

        if (static_cast<int32_t>(vnu::netcore::now_ms() - deadline) >= 0)
            return -VNU_ETIMEDOUT;
    }
}

long socket_close(int sock)
{
    if (sock < 0 || sock >= MAX_SOCKS || !g_socks[sock].used)
        return -VNU_EBADF;
    TcpSock& s = g_socks[sock];

    if (s.state == ST_LISTEN) {
        /* Abort every child still owned by this listener (half-open or
         * parked in its accept queue): none was ever handed out, so a
         * plain dealloc is correct — and it keeps the table from leaking. */
        for (int i = 0; i < MAX_SOCKS; ++i)
            if (g_socks[i].used && g_socks[i].listener == sock) {
                g_socks[i].used = false;
                g_socks[i].state = ST_CLOSED;
            }
        s.used = false;
        return 0;
    }

    if (s.listener >= 0) {
        /* A server child closed before accept() took it: drop it from the
         * listener's queue (or the half-open pool) and free the slot. */
        if (s.queued && g_socks[s.listener].used)
            accept_remove(g_socks[s.listener], sock);
        s.used = false;
        s.state = ST_CLOSED;
        return 0;
    }

    if (s.state == ST_ESTABLISHED || s.state == ST_CLOSE_WAIT) {
        uint32_t deadline = vnu::netcore::now_ms() + 1000u;
        while (static_cast<int32_t>(vnu::netcore::now_ms() - deadline) < 0) {
            vnu::netcore::rx_drain();
            tcp_push(s);          /* flush remaining data */
            if (s.snd_used == 0 && !s.fin_sent)
                tcp_send_fin(s);  /* FIN only after all data is on the wire */
            if (s.state == ST_ESTABLISHED && s.fin_sent)
                s.state = ST_FIN_WAIT1;
            if (s.fin_acked || s.fin_rcvd)
                break;
            tcp_retransmit(s, vnu::netcore::now_ms());
        }
    }

    s.used = false;
    s.state = ST_CLOSED;
    return 0;
}

long socket_bind(int sock, uint16_t port)
{
    TcpSock* sp = nullptr;
    if (!valid_handle(sock, &sp))
        return -VNU_EBADF;
    TcpSock& s = *sp;
    if (s.state != ST_CLOSED)
        return -VNU_EINVAL; /* bind only a fresh, unconnected socket */
    if (port == 0)
        return -VNU_EINVAL; /* ephemeral assignment not supported:
                               socket() already auto-picked a port */
    for (int i = 0; i < MAX_SOCKS; ++i)
        if (i != sock && g_socks[i].used && g_socks[i].lport == port)
            return -VNU_EADDRINUSE;
    s.lport = port;
    return 0;
}

long socket_listen(int sock, int backlog)
{
    TcpSock* sp = nullptr;
    if (!valid_handle(sock, &sp))
        return -VNU_EBADF;
    TcpSock& s = *sp;
    if (s.state != ST_CLOSED)
        return -VNU_EINVAL;
    if (backlog < 1)
        backlog = 1;
    if (backlog > MAX_ACCEPT)
        backlog = MAX_ACCEPT;
    s.backlog = backlog;
    s.accept_q_len = 0;
    s.accept_q_head = 0;
    s.state = ST_LISTEN;
    return 0;
}

long socket_accept(int sock, uint32_t* ip_be_out, uint16_t* port_out,
                   uint32_t timeout_ms)
{
    TcpSock* sp = nullptr;
    if (!valid_handle(sock, &sp))
        return -VNU_EBADF;
    TcpSock& s = *sp;
    if (s.state != ST_LISTEN)
        return -VNU_EINVAL;
    if (timeout_ms < 20)
        timeout_ms = 20;
    if (timeout_ms > 20000)
        timeout_ms = 20000;

    uint32_t deadline = vnu::netcore::now_ms() + timeout_ms;
    for (;;) {
        /* Drive the RX path so handshakes progress while we wait. */
        vnu::netcore::rx_drain();
        if (s.accept_q_len) {
            uint8_t pos = s.accept_q_head;
            int child = s.accept_q[pos];
            s.accept_q_head = static_cast<uint8_t>((pos + 1) % MAX_ACCEPT);
            --s.accept_q_len;
            TcpSock& c = g_socks[child];
            if (ip_be_out)
                *ip_be_out = c.rip;
            if (port_out)
                *port_out = c.rport;
            c.listener = -1; /* the child is now a standalone socket */
            c.queued = false;
            return child;
        }
        if (static_cast<int32_t>(vnu::netcore::now_ms() - deadline) >= 0)
            return -VNU_ETIMEDOUT;
    }
}

} // namespace vnu::tcp