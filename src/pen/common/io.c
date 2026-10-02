#include "io.h"
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>

int io_wait(int fd, short events) {
    struct pollfd event = {.fd = fd, .events = events};
    for (int attempt = 0; attempt < 100 && alive; ++attempt) {
        int result = poll(&event, 1, 100);
        if (result > 0) return 0;
        if (result < 0 && errno != EINTR) return -1;
    }
    errno = alive ? ETIMEDOUT : EINTR;
    return -1;
}

int io_read_all(int fd, void *data, size_t length) {
    uint8_t *bytes = data;
    while (length && alive) {
        ssize_t count;
        if (io_wait(fd, POLLIN) != 0) return -1;
        count = read(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t) count;
        } else if (!count || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
    }
    return length ? -1 : 0;
}

int io_write_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length && alive) {
        ssize_t count;
        if (io_wait(fd, POLLOUT) != 0) return -1;
        count = write(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t) count;
        } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
    return length ? -1 : 0;
}