#include "lab.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TIMEOUT_MS 600000L

void cli_usage(FILE *f)
{
    fputs("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
          "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
          "       myapp recv -s <session> [-p port] <relay> <file>\n"
          "\n"
          "  -s <session>     session name shared by the sender and the receiver\n"
          "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n"
          "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n"
          "  -l <loss>        probability the relay drops a packet (default: 0)\n"
          "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
          "  -d <dup>         probability the relay duplicates a packet (default: 0)\n"
          "  -p <port>        relay port (default: 4250)\n"
          "  <relay>          host name or address of the relay\n"
          "  <file>           file to send, or file to write what is received\n",
          f);
}

int cli_valid_session(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

int cli_parse_prob(const char *s, double *out)
{
    if (!s || !*s) return -1;
    char *end;
    errno    = 0;
    double v = strtod(s, &end);
    /* The negated test also rejects NaN. */
    if (errno != 0 || *end != '\0' || !(v >= 0.0 && v <= 0.5)) return -1;
    *out = v;
    return 0;
}

static int parse_range(const char *s, long lo, long hi, long *out)
{
    char *end;
    errno  = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int bad(const char *what, const char *arg)
{
    fprintf(stderr, "error: %s: '%s'\n", what, arg);
    return -1;
}

int cli_parse(int argc, char *argv[], options_t *o)
{
    memset(o, 0, sizeof *o);
    o->port       = "4250";
    o->window     = 8;
    o->timeout_ms = 250;

    if (argc < 2) {
        fprintf(stderr, "error: expected send or recv\n");
        return -1;
    }
    const char *opts;
    if (strcmp(argv[1], "send") == 0) {
        o->mode = MODE_SEND;
        opts    = "s:w:T:l:c:d:p:";
    } else if (strcmp(argv[1], "recv") == 0) {
        o->mode = MODE_RECV;
        opts    = "s:p:";
    } else {
        return bad("mode must be send or recv", argv[1]);
    }

    /* getopt sees the mode word as its program name. */
    int    ac = argc - 1;
    char **av = argv + 1;
    long   v;
    int    c;
#ifdef __GLIBC__
    optind = 0; /* full reset, so this can be called more than once */
#else
    optind = 1;
#endif
    while ((c = getopt(ac, av, opts)) != -1) {
        switch (c) {
        case 's':
            o->session = optarg;
            break;
        case 'w':
            if (parse_range(optarg, 1, GBN_MAX_WINDOW, &v)) return bad("window must be 1 to 64", optarg);
            o->window = (uint32_t)v;
            break;
        case 'T':
            if (parse_range(optarg, 1, MAX_TIMEOUT_MS, &v)) return bad("timeout must be 1 to 600000 ms", optarg);
            o->timeout_ms = v;
            break;
        case 'l':
            if (cli_parse_prob(optarg, &o->loss)) return bad("loss must be 0 to 0.5", optarg);
            break;
        case 'c':
            if (cli_parse_prob(optarg, &o->corrupt)) return bad("corrupt must be 0 to 0.5", optarg);
            break;
        case 'd':
            if (cli_parse_prob(optarg, &o->dup)) return bad("dup must be 0 to 0.5", optarg);
            break;
        case 'p':
            if (parse_range(optarg, 1, 65535, &v)) return bad("port must be 1 to 65535", optarg);
            o->port = optarg;
            break;
        default:
            return -1; /* getopt has already said what was wrong */
        }
    }

    if (!o->session) {
        fprintf(stderr, "error: -s <session> is required\n");
        return -1;
    }
    if (!cli_valid_session(o->session))
        return bad("session must be 1 to 32 of a-z, 0-9 and -", o->session);
    if (ac - optind != 2) {
        fprintf(stderr, "error: expected <relay> and <file>\n");
        return -1;
    }
    o->relay = av[optind];
    o->file  = av[optind + 1];
    return 0;
}

/* A rate as a plain decimal with no exponent and no trailing zeros: 0.1, 0.05, 0. */
static void fmt_rate(double v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%.6f", v);
    char *p = buf + strlen(buf) - 1;
    while (*p == '0') *p-- = '\0';
    if (*p == '.') *p = '\0';
}

int cli_hello(const options_t *o, char *buf, size_t cap)
{
    int n;
    if (o->mode == MODE_RECV) {
        n = snprintf(buf, cap, "HELLO %s recv", o->session);
    } else {
        char l[32], c[32], d[32];
        fmt_rate(o->loss, l, sizeof l);
        fmt_rate(o->corrupt, c, sizeof c);
        fmt_rate(o->dup, d, sizeof d);
        n = snprintf(buf, cap, "HELLO %s send %s %s %s", o->session, l, c, d);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}
