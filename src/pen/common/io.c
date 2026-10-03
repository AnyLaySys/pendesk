#define _POSIX_C_SOURCE 200809L

#include "io.h"
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint32_t) now.tv_nsec / 1000000;
}

void io_stop_process(pid_t process) {
    if (process <= 0) return;
    kill(process, SIGINT);
    for (int attempt = 0; attempt < 60; ++attempt) {
        pid_t result = waitpid(process, NULL, WNOHANG);
        if (result == process || (result < 0 && errno == ECHILD)) return;
        if (result < 0 && errno != EINTR) break;
        struct timespec delay = {.tv_nsec = 50000000};
        nanosleep(&delay, NULL);
    }
    kill(process, SIGKILL);
    while (waitpid(process, NULL, 0) < 0 && errno == EINTR) {}
}

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
        count = write(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t) count;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (io_wait(fd, POLLOUT) != 0) return -1;
        } else if (!count || errno != EINTR) return -1;
    }
    return length ? -1 : 0;
}
