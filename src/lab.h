#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ── Layer 1 : Packets ───────────────────────────────────────────────────
 *
 * Pure functions: bytes in, bytes or a struct out.  No I/O.
 *
 *   offset  size  field
 *   0       1     type: 0 DATA, 1 ACK, 2 FIN
 *   1       1     reserved, always 0
 *   2       2     checksum (RFC 1071)
 *   4       4     seq
 *   8       2     length of the payload in bytes
 *   10      len   payload, at most 1024 bytes
 *
 * Multi-byte fields are in network byte order.
 */

#define PKT_HDR_LEN     10u
#define PKT_MAX_PAYLOAD 1024u
#define PKT_MAX_LEN     (PKT_HDR_LEN + PKT_MAX_PAYLOAD)

enum { PKT_DATA = 0, PKT_ACK = 1, PKT_FIN = 2 };

/** A decoded, validated packet.  The payload is a copy, not a pointer. */
typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint16_t len;
    uint8_t  payload[PKT_MAX_PAYLOAD];
} pkt_t;

/**
 * RFC 1071 Internet checksum of buf[0..len-1]: the one's complement of the
 * one's complement sum of the bytes taken as big-endian 16-bit words.  An odd
 * length is padded with one zero byte for the calculation only.
 *
 * Run over a whole packet with its checksum field filled in, the result is 0
 * exactly when the packet is undamaged (the words sum to 0xffff).
 */
uint16_t pkt_checksum(const uint8_t *buf, size_t len);

/**
 * Encode one packet into buf, which must hold PKT_HDR_LEN + len bytes, and
 * fill in its checksum.  payload may be NULL when len is 0.
 *
 * Returns the number of bytes written, or 0 if buf is NULL, type is not
 * DATA/ACK/FIN, len exceeds PKT_MAX_PAYLOAD, or payload is NULL with len > 0.
 */
size_t pkt_encode(uint8_t *buf, uint8_t type, uint32_t seq,
                  const uint8_t *payload, size_t len);

/**
 * Decode and validate the datagram buf[0..len-1] into *out.  The datagram is
 * accepted only if it is at least PKT_HDR_LEN bytes, PKT_HDR_LEN + length
 * equals len, length is at most PKT_MAX_PAYLOAD, type is DATA/ACK/FIN,
 * reserved is 0, and the checksum verifies.
 *
 * Returns 0 if the packet is valid, -1 if it must be discarded.  The length
 * field is checked against len before a single payload byte is copied.
 */
int pkt_decode(const uint8_t *buf, size_t len, pkt_t *out);

/* ── Layer 2 : Go-Back-N state machines ──────────────────────────────────
 *
 * A sender and a receiver, each a struct plus the functions that feed it
 * events.  The current time is always a parameter in milliseconds, and every
 * event function fills an "out" struct that says what to do next: which
 * datagrams to send, what payload to deliver, and when the timer is due.
 * Nothing in this layer touches a socket, a clock, or a file.
 */

#define GBN_MAX_WINDOW   64u
#define GBN_MAX_TIMEOUTS 10u                  /* in a row, with no progress */
#define GBN_MAX_FILE     (16u * 1024u * 1024u)
#define GBN_LINGER_MS    2000
#define GBN_IDLE_MS      30000

typedef enum { GBN_RUNNING = 0, GBN_DONE = 1, GBN_FAILED = 2 } gbn_status_t;

/** One encoded datagram to put on the wire.  buf points into the owner. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
} gbn_datagram_t;

/** What the sender wants done after an event. */
typedef struct {
    gbn_datagram_t send[GBN_MAX_WINDOW]; /* send these, in order */
    size_t         nsend;
    int64_t        timer_due;            /* absolute ms, or -1 if stopped */
    gbn_status_t   status;
} gbn_sender_out_t;

typedef struct {
    const uint8_t *data;      /* the whole file, owned by the caller */
    size_t         len;
    uint32_t       ndata;     /* number of DATA packets; the FIN is seq ndata */
    uint32_t       window;
    int64_t        timeout_ms;
    uint32_t       base;      /* oldest unacknowledged packet */
    uint32_t       next;      /* next packet never sent */
    uint8_t       *slots;     /* window * PKT_MAX_LEN bytes: copies of base..next-1 */
    size_t         slot_len[GBN_MAX_WINDOW];
    int64_t        timer_due; /* -1 when the timer is stopped */
    uint32_t       timeouts;  /* consecutive timeouts without progress */
    gbn_status_t   status;
    /* Counters for reporting only. */
    uint64_t       sent;         /* every datagram handed out, resends included */
    uint64_t       resent;       /* datagrams sent again after a timeout */
    uint64_t       total_timeouts;
} gbn_sender_t;

/**
 * Prepare a sender for len bytes at data (NULL is allowed when len is 0).
 * Allocates the retransmission buffer; release it with gbn_sender_destroy.
 * Returns 0, or -1 if window is not 1..GBN_MAX_WINDOW, timeout_ms is not
 * positive, len exceeds GBN_MAX_FILE, or the allocation fails.
 */
int gbn_sender_init(gbn_sender_t *s, const uint8_t *data, size_t len,
                    uint32_t window, int64_t timeout_ms);

/** Free the retransmission buffer.  Safe on NULL and to call twice. */
void gbn_sender_destroy(gbn_sender_t *s);

/** Fill the window for the first time.  Call once, right after init. */
void gbn_sender_start(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out);

/**
 * A valid packet arrived.  Anything other than an ACK with
 * base < seq <= next is ignored.  A new cumulative ACK slides base, resets
 * the timeout count, restarts or stops the timer, and refills the window;
 * the ACK of the FIN finishes the transfer.
 */
void gbn_sender_on_ack(gbn_sender_t *s, const pkt_t *ack, int64_t now,
                       gbn_sender_out_t *out);

/**
 * Time has passed.  If the timer is running and now >= its due time, resend
 * every packet from base to next - 1 and restart the timer, or give up
 * (GBN_FAILED) on the GBN_MAX_TIMEOUTS-th timeout in a row.  Otherwise
 * nothing happens, so it is safe to call after every wakeup.
 */
void gbn_sender_on_timer(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out);

/** What the receiver wants done after an event. */
typedef struct {
    gbn_datagram_t ack;         /* send this ACK if ack.len > 0 */
    const uint8_t *deliver;     /* write these bytes to the file if non-NULL */
    size_t         deliver_len;
    int            fin;         /* 1 when the FIN was just accepted: close the file */
    int64_t        timer_due;   /* absolute ms, or -1 when finished */
    gbn_status_t   status;
} gbn_receiver_out_t;

typedef struct {
    uint32_t     expected;      /* next packet wanted */
    int          finished;      /* FIN accepted, now lingering */
    int64_t      deadline;      /* idle give-up time, or end of the linger */
    int64_t      linger_ms;
    int64_t      idle_ms;
    uint8_t      ackbuf[PKT_HDR_LEN];
    gbn_status_t status;
} gbn_receiver_t;

/**
 * Prepare a receiver that starts waiting at now.  It finishes linger_ms after
 * accepting the FIN, and gives up after idle_ms with no valid DATA or FIN.
 * The program uses GBN_LINGER_MS and GBN_IDLE_MS.
 */
void gbn_receiver_init(gbn_receiver_t *r, int64_t now, int64_t linger_ms,
                       int64_t idle_ms);

/**
 * A valid packet arrived.  DATA with seq == expected is delivered; a FIN with
 * seq == expected is accepted and starts the linger.  Every DATA or FIN is
 * answered with ACK expected, so duplicates, packets beyond a gap and repeated
 * FINs are re-ACKed and otherwise discarded.  ACKs are ignored.  out->deliver
 * points into *p, so write it out before reusing p.
 */
void gbn_receiver_on_packet(gbn_receiver_t *r, const pkt_t *p, int64_t now,
                            gbn_receiver_out_t *out);

/**
 * Time has passed.  At the deadline a lingering receiver is GBN_DONE and one
 * still waiting for data is GBN_FAILED.  Safe to call after every wakeup.
 */
void gbn_receiver_on_timer(gbn_receiver_t *r, int64_t now,
                           gbn_receiver_out_t *out);

/* ── Layer 3 : I/O ───────────────────────────────────────────────────────
 *
 * The socket, the relay hello, the poll loop, the clock, the file, and the
 * command line.  The only layer that knows any of them exist.
 */

/** Milliseconds from CLOCK_MONOTONIC. */
int64_t io_now_ms(void);

/**
 * Resolve host:port with getaddrinfo, preferring IPv4, and return a UDP socket
 * connected to it, or -1.  Connecting means send/recv talk only to the relay.
 */
int io_open(const char *host, const char *port);

/**
 * Register with the relay: send msg, then wait up to wait_ms for a reply,
 * sending again on silence, for at most attempts tries in all.
 * Returns 0 on "OK", 1 on "ERR <reason>" (reason copied to reason[0..cap-1]
 * if reason is non-NULL), or -1 if no reply came or the socket failed.
 */
int io_hello(int fd, const char *msg, int attempts, int wait_ms,
             char *reason, size_t cap);

/**
 * Read the whole of path into a malloc'd buffer the caller frees.
 * Returns 0, or -1 (message on stderr) if it cannot be read or is larger
 * than GBN_MAX_FILE.
 */
int io_read_file(const char *path, uint8_t **data, size_t *len);

/**
 * Send data[0..len-1] with Go-Back-N over the connected socket fd.
 * Returns 0 once the FIN is acknowledged, or 2 if the sender gives up,
 * the arguments are invalid, or the socket fails.
 */
int io_run_sender(int fd, const uint8_t *data, size_t len, uint32_t window,
                  int64_t timeout_ms);

/**
 * Receive a file with Go-Back-N over the connected socket fd, writing it to
 * out.  Takes ownership of out and always closes it.  Returns 0 after the
 * FIN is accepted and the linger ends, or 2 if nothing valid arrives for
 * idle_ms, the file cannot be written, or the socket fails.
 */
int io_run_receiver(int fd, FILE *out, int64_t linger_ms, int64_t idle_ms);

typedef enum { MODE_SEND = 1, MODE_RECV = 2 } app_mode_t;

typedef struct {
    app_mode_t  mode;
    const char *session;
    const char *relay;
    const char *file;
    const char *port;
    uint32_t    window;
    int64_t     timeout_ms;
    double      loss;
    double      corrupt;
    double      dup;
} options_t;

/** Print the usage message to f. */
void cli_usage(FILE *f);

/** Return 1 if s is a valid session name: 1 to 32 of a-z, 0-9 and '-'. */
int cli_valid_session(const char *s);

/** Parse a probability between 0 and 0.5 into *out.  Returns 0 or -1. */
int cli_parse_prob(const char *s, double *out);

/**
 * Parse "myapp send|recv [options] <relay> <file>" into *o, filling defaults.
 * Returns 0, or -1 with a message on stderr if the command line is wrong.
 */
int cli_parse(int argc, char *argv[], options_t *o);

/**
 * Write the relay hello for o into buf:
 *   "HELLO <session> recv"  or  "HELLO <session> send <loss> <corrupt> <dup>"
 * with the rates as plain decimals.  Returns 0, or -1 if it does not fit.
 */
int cli_hello(const options_t *o, char *buf, size_t cap);

#endif /* LAB_H */
