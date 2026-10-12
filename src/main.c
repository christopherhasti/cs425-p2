#include "lab.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

#define HELLO_ATTEMPTS 5
#define HELLO_WAIT_MS  1000

int main(int argc, char *argv[])
{
    /* No arguments at all: usage and exit 0 (this is what make leak runs). */
    if (argc == 1) {
        cli_usage(stdout);
        return 0;
    }

    options_t o;
    char      hello[128];
    if (cli_parse(argc, argv, &o) != 0 || cli_hello(&o, hello, sizeof hello) != 0) {
        cli_usage(stderr);
        return 1;
    }

    /* Open the file before registering, so a bad path fails fast. */
    uint8_t *data = NULL;
    size_t   len  = 0;
    FILE    *out  = NULL;
    if (o.mode == MODE_SEND) {
        if (io_read_file(o.file, &data, &len) != 0) return 1;
    } else {
        out = fopen(o.file, "wb");
        if (!out) {
            fprintf(stderr, "error: cannot create %s: %s\n", o.file, strerror(errno));
            return 1;
        }
    }

    int rc = 2;
    int fd = io_open(o.relay, o.port);
    if (fd >= 0) {
        char reason[256];
        int  h = io_hello(fd, hello, HELLO_ATTEMPTS, HELLO_WAIT_MS, reason, sizeof reason);
        if (h == 0 && o.mode == MODE_SEND) {
            rc = io_run_sender(fd, data, len, o.window, o.timeout_ms);
        } else if (h == 0) {
            rc  = io_run_receiver(fd, out, GBN_LINGER_MS, GBN_IDLE_MS);
            out = NULL; /* io_run_receiver closed it */
        } else if (h == 1) {
            fprintf(stderr, "relay refused: %s\n", reason);
        } else {
            fprintf(stderr, "error: no reply from the relay at %s port %s\n",
                    o.relay, o.port);
        }
        close(fd);
    }

    if (out) fclose(out);
    free(data);
    return rc;
}
