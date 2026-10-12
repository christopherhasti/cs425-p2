#include "lab.h"

#include <stdlib.h>
#include <string.h>

/* ── Sender ──────────────────────────────────────────────────────────── */

static uint8_t *slot_of(gbn_sender_t *s, uint32_t seq)
{
    return s->slots + (size_t)(seq % s->window) * PKT_MAX_LEN;
}

/* Put the stored copy of packet seq on the send list. */
static void queue_slot(gbn_sender_t *s, uint32_t seq, gbn_sender_out_t *out)
{
    out->send[out->nsend].buf = slot_of(s, seq);
    out->send[out->nsend].len = s->slot_len[seq % s->window];
    out->nsend++;
    s->sent++;
}

static void out_begin(gbn_sender_out_t *out)
{
    out->nsend = 0;
}

static void out_end(const gbn_sender_t *s, gbn_sender_out_t *out)
{
    out->timer_due = s->timer_due;
    out->status    = s->status;
}

/*
 * Send while next < base + window and there is something left to send.  The
 * FIN is packet ndata, and it goes only once every DATA packet is ACKed.
 */
static void fill_window(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out)
{
    while (s->next < s->base + s->window) {
        uint8_t *slot = slot_of(s, s->next);
        size_t   n;

        if (s->next < s->ndata) {
            size_t off   = (size_t)s->next * PKT_MAX_PAYLOAD;
            size_t chunk = s->len - off;
            if (chunk > PKT_MAX_PAYLOAD) chunk = PKT_MAX_PAYLOAD;
            n = pkt_encode(slot, PKT_DATA, s->next, s->data + off, chunk);
        } else if (s->next == s->ndata && s->base == s->ndata) {
            n = pkt_encode(slot, PKT_FIN, s->next, NULL, 0);
        } else {
            break;
        }
        s->slot_len[s->next % s->window] = n;
        queue_slot(s, s->next, out);
        s->next++;
    }
    if (s->timer_due < 0 && s->base < s->next) s->timer_due = now + s->timeout_ms;
}

int gbn_sender_init(gbn_sender_t *s, const uint8_t *data, size_t len,
                    uint32_t window, int64_t timeout_ms)
{
    if (!s) return -1;
    memset(s, 0, sizeof *s);
    if (window < 1 || window > GBN_MAX_WINDOW || timeout_ms <= 0) return -1;
    if (len > GBN_MAX_FILE || (!data && len > 0)) return -1;

    s->slots = malloc((size_t)window * PKT_MAX_LEN);
    if (!s->slots) return -1; /* GCOVR_EXCL_LINE */

    s->data       = data;
    s->len        = len;
    s->ndata      = (uint32_t)((len + PKT_MAX_PAYLOAD - 1u) / PKT_MAX_PAYLOAD);
    s->window     = window;
    s->timeout_ms = timeout_ms;
    s->timer_due  = -1;
    s->status     = GBN_RUNNING;
    return 0;
}

void gbn_sender_destroy(gbn_sender_t *s)
{
    if (!s) return;
    free(s->slots);
    s->slots = NULL;
}

void gbn_sender_start(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out)
{
    out_begin(out);
    fill_window(s, now, out);
    out_end(s, out);
}

void gbn_sender_on_ack(gbn_sender_t *s, const pkt_t *ack, int64_t now,
                       gbn_sender_out_t *out)
{
    out_begin(out);
    /* seq <= base is a duplicate; seq > next acknowledges something never sent. */
    if (s->status == GBN_RUNNING && ack && ack->type == PKT_ACK &&
        ack->seq > s->base && ack->seq <= s->next) {
        s->base     = ack->seq; /* cumulative: may slide several packets at once */
        s->timeouts = 0;
        if (s->base == s->ndata + 1u) {
            s->status    = GBN_DONE; /* that was the ACK of the FIN */
            s->timer_due = -1;
        } else {
            s->timer_due = s->base < s->next ? now + s->timeout_ms : -1;
            fill_window(s, now, out);
        }
    }
    out_end(s, out);
}

void gbn_sender_on_timer(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out)
{
    out_begin(out);
    if (s->status == GBN_RUNNING && s->timer_due >= 0 && now >= s->timer_due) {
        s->timeouts++;
        s->total_timeouts++;
        if (s->timeouts >= GBN_MAX_TIMEOUTS) {
            s->status    = GBN_FAILED;
            s->timer_due = -1;
        } else {
            /* Go back N: everything still in flight goes again. */
            for (uint32_t seq = s->base; seq < s->next; seq++) {
                queue_slot(s, seq, out);
                s->resent++;
            }
            s->timer_due = now + s->timeout_ms;
        }
    }
    out_end(s, out);
}

/* ── Receiver ────────────────────────────────────────────────────────── */

static void recv_out_end(const gbn_receiver_t *r, gbn_receiver_out_t *out)
{
    out->timer_due = r->status == GBN_RUNNING ? r->deadline : -1;
    out->status    = r->status;
}

void gbn_receiver_init(gbn_receiver_t *r, int64_t now, int64_t linger_ms,
                       int64_t idle_ms)
{
    memset(r, 0, sizeof *r);
    r->linger_ms = linger_ms;
    r->idle_ms   = idle_ms;
    r->deadline  = now + idle_ms;
    r->status    = GBN_RUNNING;
}

void gbn_receiver_on_packet(gbn_receiver_t *r, const pkt_t *p, int64_t now,
                            gbn_receiver_out_t *out)
{
    memset(out, 0, sizeof *out);
    if (r->status != GBN_RUNNING || !p || p->type == PKT_ACK) {
        recv_out_end(r, out);
        return;
    }

    if (!r->finished) {
        r->deadline = now + r->idle_ms;
        if (p->seq == r->expected) {
            if (p->type == PKT_DATA) {
                out->deliver     = p->payload;
                out->deliver_len = p->len;
            } else {
                r->finished = 1;
                r->deadline = now + r->linger_ms;
                out->fin    = 1;
            }
            r->expected++;
        }
    }

    /* In order or not, the answer is always "next I want is expected". */
    out->ack.len = pkt_encode(r->ackbuf, PKT_ACK, r->expected, NULL, 0);
    out->ack.buf = r->ackbuf;
    recv_out_end(r, out);
}

void gbn_receiver_on_timer(gbn_receiver_t *r, int64_t now,
                           gbn_receiver_out_t *out)
{
    memset(out, 0, sizeof *out);
    if (r->status == GBN_RUNNING && now >= r->deadline)
        r->status = r->finished ? GBN_DONE : GBN_FAILED;
    recv_out_end(r, out);
}
