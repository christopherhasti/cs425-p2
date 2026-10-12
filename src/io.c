#include "lab.h"

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t io_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int io_open(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "error: cannot resolve %s port %s: %s\n", host, port,
                gai_strerror(rc));
        return -1;
    }

    /* The relay listens on IPv4 only, so prefer an IPv4 address if there is one. */
    struct addrinfo *ai = res;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            ai = p;
            break;
        }
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { /* GCOVR_EXCL_START */
        perror("socket");
        if (fd >= 0) close(fd);
        fd = -1;
    } /* GCOVR_EXCL_STOP */
    freeaddrinfo(res);
    return fd;
}

/*
 * Errors that mean "this datagram is gone" rather than "the socket is broken".
 * ECONNREFUSED is an ICMP port unreachable about an earlier datagram: nobody
 * is listening at the relay's port right now.  UDP promises nothing, so the
 * protocol treats all of these exactly like a loss.
 */
static int transient(int e)
{
    return e == ECONNREFUSED || e == EINTR || e == EAGAIN || e == ENOBUFS;
}

/* Wait until fd is readable or the clock reaches due (-1: stopped, wait 1 s).
 * Returns 1 if readable, 0 if the time ran out, -1 if poll failed. */
static int wait_until(int fd, int64_t due)
{
    int64_t left = due < 0 ? 1000 : due - io_now_ms();
    if (left < 0) left = 0;

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int n = poll(&pfd, 1, (int)left);
    if (n < 0) { /* GCOVR_EXCL_START */
        if (errno == EINTR) return 0;
        perror("poll");
        return -1;
    } /* GCOVR_EXCL_STOP */
    return n > 0;
}

/* Receive one datagram.  Returns 1 with *p filled if it is a valid packet,
 * 0 if it was damaged or nothing usable arrived, -1 if the socket failed. */
static int recv_packet(int fd, pkt_t *p)
{
    /* One spare byte, so an oversized datagram can never pass as a full one. */
    uint8_t buf[PKT_MAX_LEN + 1u];
    ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n < 0) {
        if (transient(errno)) return 0;
        perror("recv"); /* GCOVR_EXCL_LINE */
        return -1;      /* GCOVR_EXCL_LINE */
    }
    return pkt_decode(buf, (size_t)n, p) == 0;
}

static int send_datagrams(int fd, const gbn_datagram_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (send(fd, d[i].buf, d[i].len, 0) < 0 && !transient(errno)) {
            perror("send"); /* GCOVR_EXCL_LINE */
            return -1;      /* GCOVR_EXCL_LINE */
        }
    }
    return 0;
}

int io_hello(int fd, const char *msg, int attempts, int wait_ms,
             char *reason, size_t cap)
{
    size_t mlen = strlen(msg);

    for (int a = 0; a < attempts; a++) {
        if (send(fd, msg, mlen, 0) < 0 && !transient(errno)) {
            perror("send"); /* GCOVR_EXCL_LINE */
            return -1;      /* GCOVR_EXCL_LINE */
        }
        int64_t due = io_now_ms() + wait_ms;
        while (io_now_ms() < due) {
            int ready = wait_until(fd, due);
            if (ready < 0) return -1; /* GCOVR_EXCL_LINE */
            if (ready == 0) continue;

            char    buf[256];
            ssize_t n = recv(fd, buf, sizeof buf - 1u, MSG_DONTWAIT);
            if (n < 0) {
                if (transient(errno)) continue;
                perror("recv"); /* GCOVR_EXCL_LINE */
                return -1;      /* GCOVR_EXCL_LINE */
            }
            buf[n] = '\0';
            if (strcmp(buf, "OK") == 0) return 0;
            if (strncmp(buf, "ERR", 3) == 0) {
                if (reason) snprintf(reason, cap, "%s", buf[3] == ' ' ? buf + 4 : buf + 3);
                return 1;
            }
            /* Anything else is not an answer to our hello: keep waiting. */
        }
    }
    return -1;
}

int io_read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0) { /* GCOVR_EXCL_START */
        fprintf(stderr, "error: cannot read %s\n", path);
        fclose(f);
        return -1;
    } /* GCOVR_EXCL_STOP */
    if ((unsigned long)size > GBN_MAX_FILE) {
        fprintf(stderr, "error: %s is larger than 16 MiB\n", path);
        fclose(f);
        return -1;
    }
    rewind(f);

    size_t   n   = (size_t)size;
    uint8_t *buf = malloc(n > 0 ? n : 1u);
    if (!buf || fread(buf, 1, n, f) != n) { /* GCOVR_EXCL_START */
        fprintf(stderr, "error: cannot read %s\n", path);
        free(buf);
        fclose(f);
        return -1;
    } /* GCOVR_EXCL_STOP */
    fclose(f);
    *data = buf;
    *len  = n;
    return 0;
}

int io_run_sender(int fd, const uint8_t *data, size_t len, uint32_t window,
                  int64_t timeout_ms)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    pkt_t            pkt;
    int              rc = 2;

    if (gbn_sender_init(&s, data, len, window, timeout_ms) != 0) {
        fprintf(stderr, "error: invalid sender arguments\n");
        return 2;
    }

    gbn_sender_start(&s, io_now_ms(), &out);
    if (send_datagrams(fd, out.send, out.nsend) != 0) goto done; /* GCOVR_EXCL_LINE */

    while (out.status == GBN_RUNNING) {
        int ready = wait_until(fd, out.timer_due);
        if (ready < 0) goto done; /* GCOVR_EXCL_LINE */
        if (ready > 0) {
            int got = recv_packet(fd, &pkt);
            if (got < 0) goto done; /* GCOVR_EXCL_LINE */
            if (got > 0) {
                gbn_sender_on_ack(&s, &pkt, io_now_ms(), &out);
                if (send_datagrams(fd, out.send, out.nsend) != 0) goto done; /* GCOVR_EXCL_LINE */
            }
        }
        gbn_sender_on_timer(&s, io_now_ms(), &out);
        if (send_datagrams(fd, out.send, out.nsend) != 0) goto done; /* GCOVR_EXCL_LINE */
    }

    if (out.status == GBN_DONE) {
        fprintf(stderr,
                "sent %zu bytes in %" PRIu32 " packets: %" PRIu64 " datagrams, "
                "%" PRIu64 " resent, %" PRIu64 " timeouts\n",
                len, s.ndata + 1u, s.sent, s.resent, s.total_timeouts);
        rc = 0;
    } else {
        fprintf(stderr, "giving up: %u timeouts in a row with no progress\n",
                GBN_MAX_TIMEOUTS);
    }
done:
    gbn_sender_destroy(&s);
    return rc;
}

int io_run_receiver(int fd, FILE *out_file, int64_t linger_ms, int64_t idle_ms)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    pkt_t              pkt;
    FILE              *f     = out_file;
    size_t             total = 0;
    int                rc    = 2;

    gbn_receiver_init(&r, io_now_ms(), linger_ms, idle_ms);
    gbn_receiver_on_timer(&r, io_now_ms(), &out);

    while (out.status == GBN_RUNNING) {
        int ready = wait_until(fd, out.timer_due);
        if (ready < 0) goto done; /* GCOVR_EXCL_LINE */
        if (ready > 0) {
            int got = recv_packet(fd, &pkt);
            if (got < 0) goto done; /* GCOVR_EXCL_LINE */
            if (got > 0) {
                gbn_receiver_on_packet(&r, &pkt, io_now_ms(), &out);
                if (out.deliver) {
                    if (fwrite(out.deliver, 1, out.deliver_len, f) != out.deliver_len) {
                        perror("write");
                        goto done;
                    }
                    total += out.deliver_len;
                }
                if (out.fin) {
                    int bad = fclose(f) != 0;
                    f = NULL;
                    if (bad) {
                        perror("close");
                        goto done;
                    }
                }
                if (send_datagrams(fd, &out.ack, out.ack.len > 0) != 0) goto done; /* GCOVR_EXCL_LINE */
            }
        }
        gbn_receiver_on_timer(&r, io_now_ms(), &out);
    }

    if (out.status == GBN_DONE) {
        fprintf(stderr, "received %zu bytes\n", total);
        rc = 0;
    } else {
        fprintf(stderr, "giving up: nothing valid arrived for %" PRId64 " ms\n",
                idle_ms);
    }
done:
    if (f) fclose(f);
    return rc;
}
