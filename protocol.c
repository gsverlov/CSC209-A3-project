/*
 * protocol.c – Robust read/write wrappers for pipe communication.
 *
 * These functions handle partial reads/writes and EINTR, which can
 * occur when the OS delivers a signal during a blocking I/O call.
 * They form the reliable transport layer of our pipe protocol.
 */

#include "montecarlo.h"

/*
 * write_all – Write exactly `len` bytes to file descriptor `fd`.
 *
 * Returns 0 on success, -1 on error (errno is set).
 * Handles partial writes and EINTR transparently.
 */
int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;   /* interrupted by signal, retry */
            }
            return -1;      /* real error */
        }
        if (n == 0) {
            /* write returned 0 – should not happen on a pipe,
             * but treat as error to be safe */
            errno = EIO;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/*
 * read_all – Read exactly `len` bytes from file descriptor `fd`.
 *
 * Returns 0 on success, -1 on error, 1 on clean EOF (0 bytes read
 * on the very first call).  Handles partial reads and EINTR.
 */
int read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t remaining = len;
    int first = 1;

    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            if (first) {
                return 1;   /* clean EOF: pipe closed, no data */
            }
            /* partial message then EOF – protocol error */
            errno = ECONNRESET;
            return -1;
        }
        first = 0;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}
