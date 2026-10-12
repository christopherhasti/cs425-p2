#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "harness/unity.h"
#include "../src/lab.h"

void setUp(void) {}
void tearDown(void) {}

/* ═══════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* xorshift64: the same seed gives the same run on every machine. */
static uint64_t rng_next(uint64_t *st)
{
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *st = x;
}

static double rng_unit(uint64_t *st)
{
    return (double)(rng_next(st) >> 11) / 9007199254740992.0; /* [0, 1) */
}

static void fill_random(uint8_t *buf, size_t n, uint64_t seed)
{
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rng_next(&seed);
}

/* A raw datagram with any header values at all, and a correct checksum. */
static size_t raw_packet(uint8_t *buf, uint8_t type, uint8_t reserved,
                         uint32_t seq, uint16_t lenfield,
                         const uint8_t *payload, size_t plen)
{
    buf[0] = type;
    buf[1] = reserved;
    buf[2] = 0;
    buf[3] = 0;
    buf[4] = (uint8_t)(seq >> 24);
    buf[5] = (uint8_t)(seq >> 16);
    buf[6] = (uint8_t)(seq >> 8);
    buf[7] = (uint8_t)seq;
    buf[8] = (uint8_t)(lenfield >> 8);
    buf[9] = (uint8_t)lenfield;
    if (plen > 0) memcpy(buf + PKT_HDR_LEN, payload, plen);
    size_t   total = PKT_HDR_LEN + plen;
    uint16_t c     = pkt_checksum(buf, total);
    buf[2] = (uint8_t)(c >> 8);
    buf[3] = (uint8_t)c;
    return total;
}

static pkt_t make_pkt(uint8_t type, uint32_t seq, const char *payload)
{
    pkt_t p;
    memset(&p, 0, sizeof p);
    p.type = type;
    p.seq  = seq;
    if (payload) {
        p.len = (uint16_t)strlen(payload);
        memcpy(p.payload, payload, p.len);
    }
    return p;
}

/* Decode datagram i of a sender's output and check its type and seq. */
static void assert_sent(const gbn_sender_out_t *out, size_t i, uint8_t type,
                        uint32_t seq)
{
    pkt_t p;
    TEST_ASSERT_TRUE(i < out->nsend);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(out->send[i].buf, out->send[i].len, &p));
    TEST_ASSERT_EQUAL_UINT8(type, p.type);
    TEST_ASSERT_EQUAL_UINT32(seq, p.seq);
}

static void assert_ack(const gbn_receiver_out_t *out, uint32_t seq)
{
    pkt_t p;
    TEST_ASSERT_EQUAL_size_t(PKT_HDR_LEN, out->ack.len);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(out->ack.buf, out->ack.len, &p));
    TEST_ASSERT_EQUAL_UINT8(PKT_ACK, p.type);
    TEST_ASSERT_EQUAL_UINT32(seq, p.seq);
    TEST_ASSERT_EQUAL_UINT16(0, p.len);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 1: packets
 * ═══════════════════════════════════════════════════════════════════ */

static const uint8_t HI_PACKET[] = {
    0x00, 0x00, 0x96, 0x91, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x48, 0x69, 0x21,
};

void test_checksum_rfc1071_example(void)
{
    const uint8_t b[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    /* sum 0xddf2, complement 0x220d */
    TEST_ASSERT_EQUAL_HEX16(0x220d, pkt_checksum(b, sizeof b));
}

void test_checksum_odd_length(void)
{
    const uint8_t one[]   = { 0x01 };
    const uint8_t three[] = { 0xab, 0xcd, 0xef };
    TEST_ASSERT_EQUAL_HEX16(0xfeff, pkt_checksum(one, 1));     /* ~0x0100 */
    TEST_ASSERT_EQUAL_HEX16(0x6531, pkt_checksum(three, 3));   /* ~(0xabcd + 0xef00 folded) */
    TEST_ASSERT_EQUAL_HEX16(0xffff, pkt_checksum(one, 0));     /* nothing at all */
}

void test_checksum_folds_carry(void)
{
    const uint8_t b[] = { 0xff, 0xff, 0xff, 0xff };
    TEST_ASSERT_EQUAL_HEX16(0x0000, pkt_checksum(b, sizeof b));
}

void test_checksum_of_good_packet_is_zero(void)
{
    /* With the checksum in place an undamaged packet sums to 0xffff. */
    TEST_ASSERT_EQUAL_HEX16(0x0000, pkt_checksum(HI_PACKET, sizeof HI_PACKET));
}

void test_encode_hi_example(void)
{
    uint8_t buf[PKT_MAX_LEN];
    size_t  n = pkt_encode(buf, PKT_DATA, 2, (const uint8_t *)"Hi!", 3);
    TEST_ASSERT_EQUAL_size_t(13, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(HI_PACKET, buf, 13);
}

void test_encode_ack_example(void)
{
    const uint8_t want[] = { 0x01, 0x00, 0xfe, 0xfc, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00 };
    uint8_t       buf[PKT_MAX_LEN];
    TEST_ASSERT_EQUAL_size_t(10, pkt_encode(buf, PKT_ACK, 3, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, 10);
}

void test_encode_network_byte_order(void)
{
    uint8_t buf[PKT_MAX_LEN];
    uint8_t payload[PKT_MAX_PAYLOAD];
    memset(payload, 0x5a, sizeof payload);
    TEST_ASSERT_EQUAL_size_t(PKT_MAX_LEN,
                             pkt_encode(buf, PKT_DATA, 0x01020304u, payload, PKT_MAX_PAYLOAD));
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[8]); /* 1024 = 0x0400 */
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[9]);
    TEST_ASSERT_EQUAL_HEX16(0, pkt_checksum(buf, PKT_MAX_LEN));
}

void test_encode_rejects_bad_arguments(void)
{
    uint8_t buf[PKT_MAX_LEN + 1];
    uint8_t payload[PKT_MAX_PAYLOAD + 1] = { 0 };
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(NULL, PKT_DATA, 0, payload, 1));
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(buf, 3, 0, payload, 1));
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(buf, PKT_DATA, 0, payload, PKT_MAX_PAYLOAD + 1));
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(buf, PKT_DATA, 0, NULL, 1));
}

void test_decode_round_trip(void)
{
    pkt_t p;
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(HI_PACKET, sizeof HI_PACKET, &p));
    TEST_ASSERT_EQUAL_UINT8(PKT_DATA, p.type);
    TEST_ASSERT_EQUAL_UINT32(2, p.seq);
    TEST_ASSERT_EQUAL_UINT16(3, p.len);
    TEST_ASSERT_EQUAL_MEMORY("Hi!", p.payload, 3);

    uint8_t buf[PKT_MAX_LEN];
    size_t  n = pkt_encode(buf, PKT_FIN, 77, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(buf, n, &p));
    TEST_ASSERT_EQUAL_UINT8(PKT_FIN, p.type);
    TEST_ASSERT_EQUAL_UINT32(77, p.seq);
    TEST_ASSERT_EQUAL_UINT16(0, p.len);
}

void test_decode_rejects_every_single_bit_flip(void)
{
    uint8_t buf[sizeof HI_PACKET];
    pkt_t   p;
    for (size_t bit = 0; bit < sizeof HI_PACKET * 8u; bit++) {
        memcpy(buf, HI_PACKET, sizeof buf);
        buf[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pkt_decode(buf, sizeof buf, &p), "flipped bit accepted");
    }
}

void test_decode_rejects_too_short(void)
{
    pkt_t p;
    for (size_t n = 0; n < PKT_HDR_LEN; n++)
        TEST_ASSERT_EQUAL_INT(-1, pkt_decode(HI_PACKET, n, &p));
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(NULL, 13, &p));
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(HI_PACKET, 13, NULL));
}

void test_decode_rejects_length_mismatch(void)
{
    uint8_t buf[PKT_MAX_LEN + 8];
    pkt_t   p;
    memcpy(buf, HI_PACKET, sizeof HI_PACKET);
    buf[sizeof HI_PACKET] = 0;
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, 12, &p)); /* says 3, has 2 */
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, 14, &p)); /* says 3, has 4 */

    /* Checksum is right, but the length field disagrees with the size. */
    size_t n = raw_packet(buf, PKT_DATA, 0, 1, 5, (const uint8_t *)"abc", 3);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));

    /* Consistent, checksummed, and one byte too long. */
    uint8_t big[PKT_MAX_PAYLOAD + 1];
    memset(big, 7, sizeof big);
    n = raw_packet(buf, PKT_DATA, 0, 1, PKT_MAX_PAYLOAD + 1, big, sizeof big);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
}

void test_decode_rejects_unknown_type_and_reserved(void)
{
    uint8_t buf[PKT_MAX_LEN];
    pkt_t   p;
    size_t  n = raw_packet(buf, 3, 0, 0, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
    n = raw_packet(buf, 0xff, 0, 0, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
    n = raw_packet(buf, PKT_ACK, 1, 0, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
    n = raw_packet(buf, PKT_ACK, 0, 0, 0, NULL, 0); /* the control: this one is fine */
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(buf, n, &p));
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 2: sender
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t FILE_BUF[64 * 1024 + 123];

void test_sender_init_rejects_bad_arguments(void)
{
    gbn_sender_t s;
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(NULL, FILE_BUF, 10, 4, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, 10, 0, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, 10, GBN_MAX_WINDOW + 1, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, 10, 4, 0));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, NULL, 10, 4, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, GBN_MAX_FILE + 1u, 4, 250));
    gbn_sender_destroy(&s); /* nothing was allocated; still safe */
    gbn_sender_destroy(NULL);
}

void test_sender_empty_file_is_a_single_fin(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, NULL, 0, 4, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 0);
    TEST_ASSERT_EQUAL_INT64(250, out.timer_due);

    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, 40, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    gbn_sender_destroy(&s);
    gbn_sender_destroy(&s); /* twice is fine */
}

void test_sender_cuts_2500_bytes_like_example_1(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    pkt_t            p;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 2500, 8, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(3, out.nsend); /* the FIN waits for every ACK */
    const uint16_t lens[] = { 1024, 1024, 452 };
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(0, pkt_decode(out.send[i].buf, out.send[i].len, &p));
        TEST_ASSERT_EQUAL_UINT8(PKT_DATA, p.type);
        TEST_ASSERT_EQUAL_UINT32(i, p.seq);
        TEST_ASSERT_EQUAL_UINT16(lens[i], p.len);
        TEST_ASSERT_EQUAL_MEMORY(FILE_BUF + i * 1024u, p.payload, p.len);
    }
    pkt_t ack = make_pkt(PKT_ACK, 3, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 3);
    ack.seq = 4;
    gbn_sender_on_ack(&s, &ack, 200, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    gbn_sender_destroy(&s);
}

void test_sender_exact_multiple_of_1024(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 2048, 8, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(2, out.nsend); /* no empty third DATA packet */
    TEST_ASSERT_EQUAL_size_t(PKT_MAX_LEN, out.send[1].len);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    assert_sent(&out, 0, PKT_FIN, 2);
    gbn_sender_destroy(&s);
}

void test_sender_stops_when_window_is_full(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 1000, &out);
    TEST_ASSERT_EQUAL_size_t(4, out.nsend);
    for (uint32_t i = 0; i < 4; i++) assert_sent(&out, i, PKT_DATA, i);
    TEST_ASSERT_EQUAL_UINT32(0, s.base);
    TEST_ASSERT_EQUAL_UINT32(4, s.next);
    TEST_ASSERT_EQUAL_INT64(1250, out.timer_due);

    gbn_sender_on_timer(&s, 1249, &out); /* not due yet: nothing happens */
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_INT64(1250, out.timer_due);
    gbn_sender_destroy(&s);
}

void test_sender_cumulative_ack_slides_several(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 0, &out);

    pkt_t ack = make_pkt(PKT_ACK, 3, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    TEST_ASSERT_EQUAL_UINT32(3, s.base);
    TEST_ASSERT_EQUAL_size_t(3, out.nsend); /* window slid by three */
    for (uint32_t i = 0; i < 3; i++) assert_sent(&out, i, PKT_DATA, 4 + i);
    TEST_ASSERT_EQUAL_INT64(350, out.timer_due); /* restarted */

    /* Everything in flight acknowledged at once: timer restarts for the refill. */
    ack.seq = 7;
    gbn_sender_on_ack(&s, &ack, 200, &out);
    TEST_ASSERT_EQUAL_size_t(3, out.nsend);
    assert_sent(&out, 2, PKT_DATA, 9);
    TEST_ASSERT_EQUAL_INT64(450, out.timer_due);
    gbn_sender_destroy(&s);
}

void test_sender_lost_acks_cost_nothing_example_3(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 5 * 1024, 3, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 10, &out); /* base 2, next 5 */
    TEST_ASSERT_EQUAL_UINT32(5, s.next);
    ack.seq = 5; /* ACK 3 and ACK 4 were dropped */
    gbn_sender_on_ack(&s, &ack, 20, &out);
    TEST_ASSERT_EQUAL_UINT32(5, s.base);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 5); /* nothing resent, straight to the FIN */
    TEST_ASSERT_EQUAL_UINT64(0, s.resent);
    gbn_sender_destroy(&s);
}

void test_sender_ignores_duplicate_and_bogus_acks(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 50, &out);
    TEST_ASSERT_EQUAL_size_t(2, out.nsend);

    gbn_sender_on_ack(&s, &ack, 60, &out); /* duplicate ACK 2 */
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);
    TEST_ASSERT_EQUAL_INT64(300, out.timer_due); /* not restarted */

    ack.seq = 1; /* older still */
    gbn_sender_on_ack(&s, &ack, 70, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);

    ack.seq = 9; /* beyond next: was never sent */
    gbn_sender_on_ack(&s, &ack, 80, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);

    pkt_t data = make_pkt(PKT_DATA, 4, "x"); /* not an ACK */
    gbn_sender_on_ack(&s, &data, 90, &out);
    gbn_sender_on_ack(&s, NULL, 90, &out);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    gbn_sender_destroy(&s);
}

void test_sender_timeout_resends_whole_window(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out); /* base 2, next 6, due 350 */

    gbn_sender_on_timer(&s, 350, &out);
    TEST_ASSERT_EQUAL_size_t(4, out.nsend); /* go back to base: 2, 3, 4, 5 */
    for (uint32_t i = 0; i < 4; i++) assert_sent(&out, i, PKT_DATA, 2 + i);
    TEST_ASSERT_EQUAL_INT64(600, out.timer_due);
    TEST_ASSERT_EQUAL_UINT64(4, s.resent);
    TEST_ASSERT_EQUAL_UINT32(1, s.timeouts);
    gbn_sender_destroy(&s);
}

void test_sender_gives_up_after_10_timeouts(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 5 * 1024, 2, 100));
    gbn_sender_start(&s, 0, &out);
    int64_t now = 0;
    for (int i = 1; i < (int)GBN_MAX_TIMEOUTS; i++) {
        now = out.timer_due;
        gbn_sender_on_timer(&s, now, &out);
        TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
        TEST_ASSERT_EQUAL_size_t(2, out.nsend);
    }
    gbn_sender_on_timer(&s, out.timer_due, &out); /* the 10th */
    TEST_ASSERT_EQUAL_INT(GBN_FAILED, out.status);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);

    /* A late ACK changes nothing once the sender has given up. */
    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, now + 1000, &out);
    TEST_ASSERT_EQUAL_INT(GBN_FAILED, out.status);
    gbn_sender_on_timer(&s, now + 5000, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    gbn_sender_destroy(&s);
}

void test_sender_progress_resets_timeout_count(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 5 * 1024, 2, 100));
    gbn_sender_start(&s, 0, &out);
    for (int i = 1; i < (int)GBN_MAX_TIMEOUTS; i++) gbn_sender_on_timer(&s, out.timer_due, &out);
    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, out.timer_due - 1, &out);
    TEST_ASSERT_EQUAL_UINT32(0, s.timeouts);
    for (int i = 1; i < (int)GBN_MAX_TIMEOUTS; i++) gbn_sender_on_timer(&s, out.timer_due, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    TEST_ASSERT_EQUAL_UINT64(18, s.total_timeouts);
    gbn_sender_destroy(&s);
}

void test_sender_retransmits_fin_until_acked(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 100, 1, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    assert_sent(&out, 0, PKT_FIN, 1);
    gbn_sender_on_timer(&s, 350, &out); /* its ACK was lost */
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 1);
    ack.seq = 2;
    gbn_sender_on_ack(&s, &ack, 400, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    gbn_sender_destroy(&s);
}

void test_sender_window_1_is_stop_and_wait(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 3 * 1024, 1, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    for (uint32_t i = 1; i <= 3; i++) {
        pkt_t ack = make_pkt(PKT_ACK, i, NULL);
        gbn_sender_on_ack(&s, &ack, (int64_t)i * 100, &out);
        TEST_ASSERT_EQUAL_size_t(1, out.nsend);
        assert_sent(&out, 0, i < 3 ? PKT_DATA : PKT_FIN, i);
    }
    gbn_sender_destroy(&s);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 2: receiver
 * ═══════════════════════════════════════════════════════════════════ */

void test_receiver_delivers_in_order(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t p = make_pkt(PKT_DATA, 0, "ab");
    gbn_receiver_on_packet(&r, &p, 10, &out);
    TEST_ASSERT_EQUAL_size_t(2, out.deliver_len);
    TEST_ASSERT_EQUAL_MEMORY("ab", out.deliver, 2);
    assert_ack(&out, 1);
    TEST_ASSERT_EQUAL_INT(0, out.fin);

    p = make_pkt(PKT_DATA, 1, "cde");
    gbn_receiver_on_packet(&r, &p, 20, &out);
    TEST_ASSERT_EQUAL_MEMORY("cde", out.deliver, 3);
    assert_ack(&out, 2);
}

void test_receiver_reacks_duplicate(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t p = make_pkt(PKT_DATA, 0, "ab");
    gbn_receiver_on_packet(&r, &p, 10, &out);
    gbn_receiver_on_packet(&r, &p, 20, &out);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 1);
}

void test_receiver_discards_packet_beyond_gap(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t p = make_pkt(PKT_DATA, 1, "late");
    gbn_receiver_on_packet(&r, &p, 10, &out);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 0); /* still waiting for 0 */
    p = make_pkt(PKT_DATA, 0, "first");
    gbn_receiver_on_packet(&r, &p, 20, &out);
    TEST_ASSERT_NOT_NULL(out.deliver);
    assert_ack(&out, 1); /* 1 was thrown away, so it is wanted next */
}

void test_receiver_fin_linger_and_repeated_fin(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t d = make_pkt(PKT_DATA, 0, "x");
    gbn_receiver_on_packet(&r, &d, 10, &out);
    pkt_t fin = make_pkt(PKT_FIN, 1, NULL);
    gbn_receiver_on_packet(&r, &fin, 100, &out);
    TEST_ASSERT_EQUAL_INT(1, out.fin);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 2);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    TEST_ASSERT_EQUAL_INT64(2100, out.timer_due);

    /* The ACK of the FIN was lost and the FIN comes again. */
    gbn_receiver_on_packet(&r, &fin, 600, &out);
    TEST_ASSERT_EQUAL_INT(0, out.fin);
    assert_ack(&out, 2);
    TEST_ASSERT_EQUAL_INT64(2100, out.timer_due); /* the linger is not extended */
    gbn_receiver_on_packet(&r, &d, 700, &out);   /* a stale DATA too */
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 2);

    gbn_receiver_on_timer(&r, 2099, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    gbn_receiver_on_timer(&r, 2100, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);

    gbn_receiver_on_packet(&r, &fin, 3000, &out); /* gone: no answer */
    TEST_ASSERT_EQUAL_size_t(0, out.ack.len);
}

void test_receiver_empty_file(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t fin = make_pkt(PKT_FIN, 0, NULL);
    gbn_receiver_on_packet(&r, &fin, 5, &out);
    TEST_ASSERT_EQUAL_INT(1, out.fin);
    assert_ack(&out, 1);
}

void test_receiver_fin_out_of_order_is_reacked(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t fin = make_pkt(PKT_FIN, 2, NULL);
    gbn_receiver_on_packet(&r, &fin, 5, &out);
    TEST_ASSERT_EQUAL_INT(0, out.fin);
    assert_ack(&out, 0);
}

void test_receiver_ignores_acks(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t ack = make_pkt(PKT_ACK, 0, NULL);
    gbn_receiver_on_packet(&r, &ack, 5, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.ack.len);
    TEST_ASSERT_NULL(out.deliver);
    gbn_receiver_on_packet(&r, NULL, 5, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.ack.len);
    TEST_ASSERT_EQUAL_INT64(GBN_IDLE_MS, out.timer_due); /* not a sign of life */
}

void test_receiver_gives_up_when_idle(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    gbn_receiver_on_timer(&r, 29999, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    pkt_t d = make_pkt(PKT_DATA, 5, "x"); /* out of order, but valid: still alive */
    gbn_receiver_on_packet(&r, &d, 10000, &out);
    TEST_ASSERT_EQUAL_INT64(40000, out.timer_due);
    gbn_receiver_on_timer(&r, 39999, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    gbn_receiver_on_timer(&r, 40000, &out);
    TEST_ASSERT_EQUAL_INT(GBN_FAILED, out.status);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 2 end to end: both state machines through a lossy in-memory channel
 * ═══════════════════════════════════════════════════════════════════ */

#define CH_MAX   8192
#define CH_DELAY 50 /* ms each way, like the relay's --delay 50 */

typedef struct {
    uint8_t  buf[PKT_MAX_LEN];
    size_t   len;
    int64_t  at;
    uint64_t order;
    int      to_receiver;
} flight_t;

typedef struct {
    flight_t *q;
    size_t    n;
    uint64_t  order;
    uint64_t  rng;
    double    loss, corrupt, dup;
    unsigned  dropped, corrupted, duplicated;
} channel_t;

static void ch_push(channel_t *c, const uint8_t *buf, size_t len, int to_receiver, int64_t now)
{
    TEST_ASSERT_TRUE(c->n < CH_MAX);
    flight_t *f = &c->q[c->n++];
    memcpy(f->buf, buf, len);
    f->len         = len;
    f->at          = now + CH_DELAY;
    f->order       = c->order++;
    f->to_receiver = to_receiver;
}

/* Damage a datagram exactly the way the relay does: drop, else flip one bit,
 * else deliver twice, each independently for every datagram. */
static void ch_send(channel_t *c, const uint8_t *buf, size_t len, int to_receiver, int64_t now)
{
    if (rng_unit(&c->rng) < c->loss) {
        c->dropped++;
        return;
    }
    if (rng_unit(&c->rng) < c->corrupt) {
        uint8_t tmp[PKT_MAX_LEN];
        memcpy(tmp, buf, len);
        size_t bit = (size_t)(rng_next(&c->rng) % (len * 8u));
        tmp[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        ch_push(c, tmp, len, to_receiver, now);
        c->corrupted++;
        return;
    }
    ch_push(c, buf, len, to_receiver, now);
    if (rng_unit(&c->rng) < c->dup) {
        ch_push(c, buf, len, to_receiver, now);
        c->duplicated++;
    }
}

/* The datagram that arrives first (FIFO among equal times), or -1. */
static long ch_first(const channel_t *c)
{
    long best = -1;
    for (size_t i = 0; i < c->n; i++) {
        if (best < 0 || c->q[i].at < c->q[best].at ||
            (c->q[i].at == c->q[best].at && c->q[i].order < c->q[best].order))
            best = (long)i;
    }
    return best;
}

typedef struct {
    gbn_status_t sender, receiver;
    size_t       got_len;
    int          match;
    int          fin_seen;
    uint64_t     resent;
    int64_t      elapsed;
} transfer_t;

/* Run a whole transfer on a virtual clock, one event at a time. */
static transfer_t run_transfer(const uint8_t *data, size_t len, uint32_t window,
                               double rate, uint64_t seed)
{
    transfer_t res;
    memset(&res, 0, sizeof res);
    channel_t c;
    memset(&c, 0, sizeof c);
    c.q    = malloc(CH_MAX * sizeof *c.q);
    c.rng  = seed;
    c.loss = c.corrupt = c.dup = rate;
    TEST_ASSERT_NOT_NULL(c.q);
    uint8_t *got = malloc(len + 1u);
    TEST_ASSERT_NOT_NULL(got);

    gbn_sender_t       s;
    gbn_sender_out_t   so;
    gbn_receiver_t     r;
    gbn_receiver_out_t ro;
    pkt_t              p;
    int64_t            now = 0;

    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, data, len, window, 250));
    gbn_receiver_init(&r, now, GBN_LINGER_MS, GBN_IDLE_MS);
    gbn_receiver_on_timer(&r, now, &ro);
    gbn_sender_start(&s, now, &so);
    for (size_t i = 0; i < so.nsend; i++) ch_send(&c, so.send[i].buf, so.send[i].len, 1, now);

    for (long steps = 0; steps < 1000000; steps++) {
        if (so.status != GBN_RUNNING && ro.status != GBN_RUNNING) break;
        long    i = ch_first(&c);
        int64_t t = i >= 0 ? c.q[i].at : INT64_MAX;
        if (so.status == GBN_RUNNING && so.timer_due >= 0 && so.timer_due < t) t = so.timer_due;
        if (ro.status == GBN_RUNNING && ro.timer_due < t) t = ro.timer_due;
        TEST_ASSERT_TRUE_MESSAGE(t != INT64_MAX, "deadlock: nothing in flight and no timer");
        now = t;

        if (i >= 0 && c.q[i].at == now) {
            flight_t f = c.q[i];
            c.q[i]     = c.q[--c.n];
            if (pkt_decode(f.buf, f.len, &p) != 0) continue; /* damaged: as if lost */
            if (f.to_receiver && ro.status == GBN_RUNNING) {
                gbn_receiver_on_packet(&r, &p, now, &ro);
                if (ro.deliver) {
                    TEST_ASSERT_TRUE(res.got_len + ro.deliver_len <= len);
                    memcpy(got + res.got_len, ro.deliver, ro.deliver_len);
                    res.got_len += ro.deliver_len;
                }
                if (ro.fin) res.fin_seen++;
                if (ro.ack.len) ch_send(&c, ro.ack.buf, ro.ack.len, 0, now);
            } else if (!f.to_receiver && so.status == GBN_RUNNING) {
                gbn_sender_on_ack(&s, &p, now, &so);
                for (size_t k = 0; k < so.nsend; k++) ch_send(&c, so.send[k].buf, so.send[k].len, 1, now);
            }
        } else if (so.status == GBN_RUNNING && so.timer_due == now) {
            gbn_sender_on_timer(&s, now, &so);
            for (size_t k = 0; k < so.nsend; k++) ch_send(&c, so.send[k].buf, so.send[k].len, 1, now);
        } else {
            gbn_receiver_on_timer(&r, now, &ro);
        }
    }

    res.sender   = so.status;
    res.receiver = ro.status;
    res.match    = res.got_len == len && (len == 0 || memcmp(got, data, len) == 0);
    res.resent   = s.resent;
    res.elapsed  = now;
    gbn_sender_destroy(&s);
    free(got);
    free(c.q);
    return res;
}

static void assert_transfer_ok(transfer_t t)
{
    TEST_ASSERT_EQUAL_INT(GBN_DONE, t.sender);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, t.receiver);
    TEST_ASSERT_EQUAL_INT(1, t.fin_seen);
    TEST_ASSERT_TRUE_MESSAGE(t.match, "delivered bytes differ from the bytes sent");
}

void test_transfer_clean_channel(void)
{
    fill_random(FILE_BUF, sizeof FILE_BUF, 42);
    transfer_t t = run_transfer(FILE_BUF, 10 * 1024 + 300, 1, 0.0, 1);
    assert_transfer_ok(t);
    TEST_ASSERT_EQUAL_UINT64(0, t.resent);
    /* Stop-and-wait: 11 DATA + FIN, one 100 ms round trip each, then the linger. */
    TEST_ASSERT_EQUAL_INT64(12 * 100 - 50 + GBN_LINGER_MS, t.elapsed);

    t = run_transfer(FILE_BUF, 10 * 1024 + 300, 16, 0.0, 1);
    assert_transfer_ok(t);
}

void test_transfer_lossy_20_percent_seeded(void)
{
    /* 20% loss, 20% corruption and 20% duplication, in each direction. */
    fill_random(FILE_BUF, sizeof FILE_BUF, 7);
    const uint64_t seeds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0x5eed, 0xc5425 };
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        transfer_t t = run_transfer(FILE_BUF, sizeof FILE_BUF, 8, 0.2, seeds[i]);
        assert_transfer_ok(t);
        TEST_ASSERT_TRUE(t.resent > 0); /* the channel really did bite */
    }
}

void test_transfer_lossy_stop_and_wait_and_big_window(void)
{
    fill_random(FILE_BUF, sizeof FILE_BUF, 99);
    assert_transfer_ok(run_transfer(FILE_BUF, 20 * 1024 + 1, 1, 0.2, 11));
    assert_transfer_ok(run_transfer(FILE_BUF, sizeof FILE_BUF, GBN_MAX_WINDOW, 0.2, 12));
}

void test_transfer_lossy_empty_and_exact_multiple(void)
{
    fill_random(FILE_BUF, sizeof FILE_BUF, 3);
    assert_transfer_ok(run_transfer(NULL, 0, 8, 0.2, 21));
    assert_transfer_ok(run_transfer(FILE_BUF, 16 * 1024, 8, 0.2, 22));
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 3: I/O over real loopback sockets, with the test playing the relay
 * ═══════════════════════════════════════════════════════════════════ */

static void sleep_ms(int ms)
{
    poll(NULL, 0, ms);
}

/* A UDP socket on 127.0.0.1 that plays the relay; its port goes into port. */
static int fake_relay(char *port, size_t cap)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr *)&a, sizeof a));
    socklen_t n = sizeof a;
    TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr *)&a, &n));
    snprintf(port, cap, "%u", (unsigned)ntohs(a.sin_port));
    return fd;
}

/* Aim the relay at the client, so replies can be queued before it asks. */
static void relay_aim(int relay, int client)
{
    struct sockaddr_storage a;
    socklen_t               n = sizeof a;
    TEST_ASSERT_EQUAL_INT(0, getsockname(client, (struct sockaddr *)&a, &n));
    TEST_ASSERT_EQUAL_INT(0, connect(relay, (struct sockaddr *)&a, n));
}

static void relay_say(int relay, const char *text)
{
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)send(relay, text, strlen(text), 0));
}

static void relay_send_pkt(int relay, uint8_t type, uint32_t seq, const char *payload)
{
    uint8_t buf[PKT_MAX_LEN];
    size_t  n = pkt_encode(buf, type, seq, (const uint8_t *)payload, payload ? strlen(payload) : 0);
    TEST_ASSERT_EQUAL_INT((int)n, (int)send(relay, buf, n, 0));
}

/* Read one datagram the client sent to the relay, without waiting long. */
static ssize_t relay_read(int relay, uint8_t *buf, size_t cap)
{
    struct pollfd pfd = { .fd = relay, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 200) <= 0) return -1;
    return recv(relay, buf, cap, 0);
}

static void relay_expect_pkt(int relay, uint8_t type, uint32_t seq, pkt_t *p)
{
    uint8_t buf[PKT_MAX_LEN + 1];
    ssize_t n = relay_read(relay, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(buf, (size_t)n, p));
    TEST_ASSERT_EQUAL_UINT8(type, p->type);
    TEST_ASSERT_EQUAL_UINT32(seq, p->seq);
}

static void temp_path(char *path, size_t cap)
{
    snprintf(path, cap, "/tmp/cs425-p2-test-XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
}

void test_io_now_ms_is_monotonic(void)
{
    int64_t a = io_now_ms();
    sleep_ms(5);
    int64_t b = io_now_ms();
    TEST_ASSERT_TRUE(b >= a + 4);
}

void test_io_open_rejects_bad_port(void)
{
    TEST_ASSERT_EQUAL_INT(-1, io_open("127.0.0.1", "not-a-port"));
}

void test_io_hello_ok_by_name(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("localhost", port); /* a name, not a dotted quad */
    TEST_ASSERT_TRUE(fd >= 0);
    relay_aim(relay, fd);
    relay_say(relay, "OK");
    TEST_ASSERT_EQUAL_INT(0, io_hello(fd, "HELLO t-1 recv", 5, 1000, NULL, 0));

    char    buf[64];
    ssize_t n = relay_read(relay, (uint8_t *)buf, sizeof buf - 1);
    TEST_ASSERT_EQUAL_INT(14, (int)n);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("HELLO t-1 recv", buf); /* no newline */
    close(fd);
    close(relay);
}

void test_io_hello_err_reports_reason(void)
{
    char port[16], reason[64];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    relay_say(relay, "ERR no receiver");
    TEST_ASSERT_EQUAL_INT(1, io_hello(fd, "HELLO t-1 send 0 0 0", 5, 1000, reason, sizeof reason));
    TEST_ASSERT_EQUAL_STRING("no receiver", reason);

    relay_say(relay, "ERR");
    TEST_ASSERT_EQUAL_INT(1, io_hello(fd, "HELLO t-1 send 0 0 0", 5, 1000, reason, sizeof reason));
    TEST_ASSERT_EQUAL_STRING("", reason);
    relay_say(relay, "ERR x");
    TEST_ASSERT_EQUAL_INT(1, io_hello(fd, "HELLO t-1 send 0 0 0", 5, 1000, NULL, 0));
    close(fd);
    close(relay);
}

void test_io_hello_ignores_noise(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    relay_say(relay, "WHAT");
    relay_say(relay, "OK");
    TEST_ASSERT_EQUAL_INT(0, io_hello(fd, "HELLO t-1 recv", 5, 1000, NULL, 0));
    close(fd);
    close(relay);
}

void test_io_hello_retries_on_silence(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    TEST_ASSERT_EQUAL_INT(-1, io_hello(fd, "HELLO t-1 recv", 3, 20, NULL, 0));
    uint8_t buf[64];
    int     hellos = 0;
    while (relay_read(relay, buf, sizeof buf) > 0) hellos++;
    TEST_ASSERT_EQUAL_INT(3, hellos);
    close(fd);
    close(relay);
}

void test_io_hello_with_no_relay_running(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    close(relay); /* nobody listens there now: ICMP port unreachable */
    int fd = io_open("127.0.0.1", port);
    TEST_ASSERT_EQUAL_INT(-1, io_hello(fd, "HELLO t-1 recv", 2, 30, NULL, 0));
    close(fd);
}

void test_io_read_file(void)
{
    char path[64];
    temp_path(path, sizeof path);
    fill_random(FILE_BUF, 2500, 5);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(FILE_BUF, 1, 2500, f);
    fclose(f);

    uint8_t *data = NULL;
    size_t   len  = 0;
    TEST_ASSERT_EQUAL_INT(0, io_read_file(path, &data, &len));
    TEST_ASSERT_EQUAL_size_t(2500, len);
    TEST_ASSERT_EQUAL_MEMORY(FILE_BUF, data, 2500);
    free(data);

    f = fopen(path, "wb"); /* now empty */
    fclose(f);
    TEST_ASSERT_EQUAL_INT(0, io_read_file(path, &data, &len));
    TEST_ASSERT_EQUAL_size_t(0, len);
    free(data);

    TEST_ASSERT_EQUAL_INT(0, truncate(path, (off_t)GBN_MAX_FILE + 1));
    TEST_ASSERT_EQUAL_INT(-1, io_read_file(path, &data, &len));
    unlink(path);
    TEST_ASSERT_EQUAL_INT(-1, io_read_file(path, &data, &len)); /* gone */
}

void test_io_run_sender_clean(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    fill_random(FILE_BUF, 1500, 9);

    /* The relay's side of the conversation, queued up in advance. */
    relay_say(relay, "junk");
    relay_send_pkt(relay, PKT_ACK, 1, NULL);
    relay_send_pkt(relay, PKT_ACK, 1, NULL); /* duplicate */
    relay_send_pkt(relay, PKT_ACK, 2, NULL);
    relay_send_pkt(relay, PKT_ACK, 3, NULL);
    TEST_ASSERT_EQUAL_INT(0, io_run_sender(fd, FILE_BUF, 1500, 4, 200));

    pkt_t p;
    relay_expect_pkt(relay, PKT_DATA, 0, &p);
    TEST_ASSERT_EQUAL_MEMORY(FILE_BUF, p.payload, 1024);
    relay_expect_pkt(relay, PKT_DATA, 1, &p);
    TEST_ASSERT_EQUAL_UINT16(476, p.len);
    TEST_ASSERT_EQUAL_MEMORY(FILE_BUF + 1024, p.payload, 476);
    relay_expect_pkt(relay, PKT_FIN, 2, &p);
    close(fd);
    close(relay);
}

void test_io_run_sender_gives_up(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    TEST_ASSERT_EQUAL_INT(2, io_run_sender(fd, FILE_BUF, 3000, 4, 2));
    TEST_ASSERT_EQUAL_INT(2, io_run_sender(fd, FILE_BUF, 3000, 0, 2)); /* bad window */
    close(relay);
    /* With the relay gone the sends bounce, and it still gives up cleanly. */
    TEST_ASSERT_EQUAL_INT(2, io_run_sender(fd, FILE_BUF, 3000, 4, 2));
    close(fd);
}

void test_io_run_receiver_writes_in_order(void)
{
    char port[16], path[64];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    temp_path(path, sizeof path);

    relay_send_pkt(relay, PKT_DATA, 0, "hello ");
    relay_send_pkt(relay, PKT_DATA, 0, "hello "); /* duplicate */
    relay_send_pkt(relay, PKT_DATA, 2, "!");      /* beyond the gap */
    uint8_t bad[PKT_MAX_LEN];
    size_t  n = pkt_encode(bad, PKT_DATA, 1, (const uint8_t *)"world", 5);
    bad[12] ^= 0x10; /* one flipped bit */
    TEST_ASSERT_EQUAL_INT((int)n, (int)send(relay, bad, n, 0));
    relay_send_pkt(relay, PKT_DATA, 1, "world");
    relay_send_pkt(relay, PKT_DATA, 2, "!");
    relay_send_pkt(relay, PKT_FIN, 3, NULL);
    relay_send_pkt(relay, PKT_FIN, 3, NULL); /* its ACK was "lost" */

    FILE *f = fopen(path, "wb");
    TEST_ASSERT_EQUAL_INT(0, io_run_receiver(fd, f, 30, 1000));

    const uint32_t acks[] = { 1, 1, 1, 2, 3, 4, 4 }; /* nothing for the damaged one */
    pkt_t          p;
    for (size_t i = 0; i < sizeof acks / sizeof acks[0]; i++)
        relay_expect_pkt(relay, PKT_ACK, acks[i], &p);

    char  text[32] = { 0 };
    FILE *in       = fopen(path, "rb");
    TEST_ASSERT_EQUAL_size_t(12, fread(text, 1, sizeof text, in));
    fclose(in);
    TEST_ASSERT_EQUAL_STRING("hello world!", text);
    unlink(path);
    close(fd);
    close(relay);
}

void test_io_run_receiver_gives_up_when_idle(void)
{
    char port[16], path[64];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    temp_path(path, sizeof path);
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, fopen(path, "wb"), 30, 30));

    /* The relay vanishes after one packet: the ACK bounces, then silence. */
    relay_send_pkt(relay, PKT_DATA, 0, "x");
    close(relay);
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, fopen(path, "wb"), 30, 50));
    unlink(path);
    close(fd);
}

void test_io_run_receiver_write_failures(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);

    /* Unbuffered, so the write itself fails. */
    FILE *f = fopen("/dev/full", "wb");
    TEST_ASSERT_NOT_NULL(f);
    setvbuf(f, NULL, _IONBF, 0);
    relay_send_pkt(relay, PKT_DATA, 0, "x");
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, f, 30, 1000));

    /* Buffered, so the failure only shows when the FIN closes the file. */
    f = fopen("/dev/full", "wb");
    relay_send_pkt(relay, PKT_DATA, 0, "x");
    relay_send_pkt(relay, PKT_FIN, 1, NULL);
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, f, 30, 1000));
    close(fd);
    close(relay);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 3: command line
 * ═══════════════════════════════════════════════════════════════════ */

static int parse_args(options_t *o, int argc, const char *const *args)
{
    char *argv[32];
    for (int i = 0; i < argc; i++) argv[i] = (char *)args[i];
    argv[argc] = NULL;
    return cli_parse(argc, argv, o);
}

#define PARSE(o, ...)                                                               \
    parse_args((o), (int)(sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(char *)), \
               (const char *[]){ __VA_ARGS__ })

void test_cli_usage(void)
{
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    cli_usage(f);
    rewind(f);
    char line[128];
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof line, f));
    TEST_ASSERT_EQUAL_STRING("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n", line);
    fclose(f);
}

void test_cli_valid_session(void)
{
    TEST_ASSERT_TRUE(cli_valid_session("jdoe-1"));
    TEST_ASSERT_TRUE(cli_valid_session("a"));
    TEST_ASSERT_TRUE(cli_valid_session("abcdefghijklmnopqrstuvwxyz-01234")); /* 32 */
    TEST_ASSERT_FALSE(cli_valid_session("abcdefghijklmnopqrstuvwxyz-012345"));
    TEST_ASSERT_FALSE(cli_valid_session(""));
    TEST_ASSERT_FALSE(cli_valid_session("JDoe"));
    TEST_ASSERT_FALSE(cli_valid_session("a_b"));
    TEST_ASSERT_FALSE(cli_valid_session("a b"));
    TEST_ASSERT_FALSE(cli_valid_session(NULL));
}

void test_cli_parse_prob(void)
{
    double v = -1;
    TEST_ASSERT_EQUAL_INT(0, cli_parse_prob("0", &v));
    TEST_ASSERT_TRUE(v == 0.0);
    TEST_ASSERT_EQUAL_INT(0, cli_parse_prob("0.5", &v));
    TEST_ASSERT_TRUE(v == 0.5);
    TEST_ASSERT_EQUAL_INT(0, cli_parse_prob(".05", &v));
    TEST_ASSERT_TRUE(v == 0.05);
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("0.51", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("-0.1", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("abc", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("0.1x", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("nan", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("1e999", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob(NULL, &v));
}

void test_cli_parse_send_defaults(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "send", "-s", "jdoe-1", "127.0.0.1", "in.bin"));
    TEST_ASSERT_EQUAL_INT(MODE_SEND, o.mode);
    TEST_ASSERT_EQUAL_STRING("jdoe-1", o.session);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", o.relay);
    TEST_ASSERT_EQUAL_STRING("in.bin", o.file);
    TEST_ASSERT_EQUAL_STRING("4250", o.port);
    TEST_ASSERT_EQUAL_UINT32(8, o.window);
    TEST_ASSERT_EQUAL_INT64(250, o.timeout_ms);
    TEST_ASSERT_TRUE(o.loss == 0.0);
    TEST_ASSERT_TRUE(o.corrupt == 0.0);
    TEST_ASSERT_TRUE(o.dup == 0.0);
}

void test_cli_parse_send_all_options(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "send", "-s", "jdoe-1", "-w", "16", "-T", "100",
                                   "-l", "0.1", "-c", "0.05", "-d", "0.2", "-p", "20000",
                                   "relay.example.com", "in.bin"));
    TEST_ASSERT_EQUAL_UINT32(16, o.window);
    TEST_ASSERT_EQUAL_INT64(100, o.timeout_ms);
    TEST_ASSERT_TRUE(o.loss == 0.1);
    TEST_ASSERT_TRUE(o.corrupt == 0.05);
    TEST_ASSERT_TRUE(o.dup == 0.2);
    TEST_ASSERT_EQUAL_STRING("20000", o.port);
    TEST_ASSERT_EQUAL_STRING("relay.example.com", o.relay);
}

void test_cli_parse_recv(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "recv", "-s", "jdoe-1", "-p", "30000", "127.0.0.1", "out.bin"));
    TEST_ASSERT_EQUAL_INT(MODE_RECV, o.mode);
    TEST_ASSERT_EQUAL_STRING("30000", o.port);
    TEST_ASSERT_EQUAL_STRING("out.bin", o.file);
}

void test_cli_parse_errors(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "bogus", "-s", "a", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "recv", "-s", "a", "-w", "4", "h", "f")); /* send-only */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "h", "f"));                       /* no -s */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "Bad_Name", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "0", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "65", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "4x", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "99999999999999999999", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-T", "0", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-l", "0.6", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-c", "x", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-d", "-1", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-p", "0", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-p", "70000", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "h"));           /* no file */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "h", "f", "x")); /* extra */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s"));                     /* -s needs a value */
    /* And a good one right after the failures still parses: getopt was reset. */
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "recv", "-s", "a", "h", "f"));
}

void test_cli_hello(void)
{
    options_t o;
    char      buf[128];
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "recv", "-s", "jdoe-1", "h", "f"));
    TEST_ASSERT_EQUAL_INT(0, cli_hello(&o, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 recv", buf);

    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "send", "-s", "jdoe-1", "-l", "0.1", "-c", ".05", "h", "f"));
    TEST_ASSERT_EQUAL_INT(0, cli_hello(&o, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 send 0.1 0.05 0", buf);

    o.dup = 0.5;
    TEST_ASSERT_EQUAL_INT(0, cli_hello(&o, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 send 0.1 0.05 0.5", buf);
    TEST_ASSERT_EQUAL_INT(-1, cli_hello(&o, buf, 10)); /* does not fit */
}

int main(void)
{
    UNITY_BEGIN();
    /* Layer 1 */
    RUN_TEST(test_checksum_rfc1071_example);
    RUN_TEST(test_checksum_odd_length);
    RUN_TEST(test_checksum_folds_carry);
    RUN_TEST(test_checksum_of_good_packet_is_zero);
    RUN_TEST(test_encode_hi_example);
    RUN_TEST(test_encode_ack_example);
    RUN_TEST(test_encode_network_byte_order);
    RUN_TEST(test_encode_rejects_bad_arguments);
    RUN_TEST(test_decode_round_trip);
    RUN_TEST(test_decode_rejects_every_single_bit_flip);
    RUN_TEST(test_decode_rejects_too_short);
    RUN_TEST(test_decode_rejects_length_mismatch);
    RUN_TEST(test_decode_rejects_unknown_type_and_reserved);
    /* Layer 2: sender */
    RUN_TEST(test_sender_init_rejects_bad_arguments);
    RUN_TEST(test_sender_empty_file_is_a_single_fin);
    RUN_TEST(test_sender_cuts_2500_bytes_like_example_1);
    RUN_TEST(test_sender_exact_multiple_of_1024);
    RUN_TEST(test_sender_stops_when_window_is_full);
    RUN_TEST(test_sender_cumulative_ack_slides_several);
    RUN_TEST(test_sender_lost_acks_cost_nothing_example_3);
    RUN_TEST(test_sender_ignores_duplicate_and_bogus_acks);
    RUN_TEST(test_sender_timeout_resends_whole_window);
    RUN_TEST(test_sender_gives_up_after_10_timeouts);
    RUN_TEST(test_sender_progress_resets_timeout_count);
    RUN_TEST(test_sender_retransmits_fin_until_acked);
    RUN_TEST(test_sender_window_1_is_stop_and_wait);
    /* Layer 2: receiver */
    RUN_TEST(test_receiver_delivers_in_order);
    RUN_TEST(test_receiver_reacks_duplicate);
    RUN_TEST(test_receiver_discards_packet_beyond_gap);
    RUN_TEST(test_receiver_fin_linger_and_repeated_fin);
    RUN_TEST(test_receiver_empty_file);
    RUN_TEST(test_receiver_fin_out_of_order_is_reacked);
    RUN_TEST(test_receiver_ignores_acks);
    RUN_TEST(test_receiver_gives_up_when_idle);
    /* Layer 2: end to end */
    RUN_TEST(test_transfer_clean_channel);
    RUN_TEST(test_transfer_lossy_20_percent_seeded);
    RUN_TEST(test_transfer_lossy_stop_and_wait_and_big_window);
    RUN_TEST(test_transfer_lossy_empty_and_exact_multiple);
    /* Layer 3 */
    RUN_TEST(test_io_now_ms_is_monotonic);
    RUN_TEST(test_io_open_rejects_bad_port);
    RUN_TEST(test_io_hello_ok_by_name);
    RUN_TEST(test_io_hello_err_reports_reason);
    RUN_TEST(test_io_hello_ignores_noise);
    RUN_TEST(test_io_hello_retries_on_silence);
    RUN_TEST(test_io_hello_with_no_relay_running);
    RUN_TEST(test_io_read_file);
    RUN_TEST(test_io_run_sender_clean);
    RUN_TEST(test_io_run_sender_gives_up);
    RUN_TEST(test_io_run_receiver_writes_in_order);
    RUN_TEST(test_io_run_receiver_gives_up_when_idle);
    RUN_TEST(test_io_run_receiver_write_failures);
    RUN_TEST(test_cli_usage);
    RUN_TEST(test_cli_valid_session);
    RUN_TEST(test_cli_parse_prob);
    RUN_TEST(test_cli_parse_send_defaults);
    RUN_TEST(test_cli_parse_send_all_options);
    RUN_TEST(test_cli_parse_recv);
    RUN_TEST(test_cli_parse_errors);
    RUN_TEST(test_cli_hello);
    return UNITY_END();
}
