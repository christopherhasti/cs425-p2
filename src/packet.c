#include "lab.h"

#include <arpa/inet.h>
#include <string.h>

uint16_t pkt_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    size_t   i;

    for (i = 0; i + 1u < len; i += 2u) {
        sum += ((uint32_t)buf[i] << 8) | buf[i + 1u];
        sum = (sum & 0xffffu) + (sum >> 16); /* fold the carry back in */
    }
    if (len & 1u) {
        sum += (uint32_t)buf[len - 1u] << 8; /* pad with one zero byte */
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

size_t pkt_encode(uint8_t *buf, uint8_t type, uint32_t seq,
                  const uint8_t *payload, size_t len)
{
    if (!buf || type > PKT_FIN || len > PKT_MAX_PAYLOAD) return 0;
    if (!payload && len > 0) return 0;

    uint32_t nseq = htonl(seq);
    uint16_t nlen = htons((uint16_t)len);

    buf[0] = type;
    buf[1] = 0;
    buf[2] = 0; /* checksum is computed with this field zero */
    buf[3] = 0;
    memcpy(buf + 4, &nseq, sizeof nseq);
    memcpy(buf + 8, &nlen, sizeof nlen);
    if (len > 0) memcpy(buf + PKT_HDR_LEN, payload, len);

    size_t   total = PKT_HDR_LEN + len;
    uint16_t ncsum = htons(pkt_checksum(buf, total));
    memcpy(buf + 2, &ncsum, sizeof ncsum);
    return total;
}

int pkt_decode(const uint8_t *buf, size_t len, pkt_t *out)
{
    if (!buf || !out || len < PKT_HDR_LEN) return -1;

    uint16_t nlen;
    memcpy(&nlen, buf + 8, sizeof nlen);
    uint16_t plen = ntohs(nlen);

    /* Trust nothing: the length field must agree with what recv returned. */
    if (plen > PKT_MAX_PAYLOAD || PKT_HDR_LEN + plen != len) return -1;
    if (buf[0] > PKT_FIN || buf[1] != 0) return -1;
    if (pkt_checksum(buf, len) != 0) return -1;

    uint32_t nseq;
    memcpy(&nseq, buf + 4, sizeof nseq);

    out->type = buf[0];
    out->seq  = ntohl(nseq);
    out->len  = plen;
    if (plen > 0) memcpy(out->payload, buf + PKT_HDR_LEN, plen);
    return 0;
}
