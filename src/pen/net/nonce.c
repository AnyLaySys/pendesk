#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "nonce.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

void nonce(uint8_t output[8]) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t length = 0;
    if (fd >= 0) {
        while (length < 8) {
            ssize_t count = read(fd, output + length, 8 - length);
            if (count > 0) length += (size_t) count;
            else if (!count || errno != EINTR) break;
        }
        close(fd);
    }
    if (length == 8) return;
    struct timespec now;
    uint64_t value = (uint64_t) getpid() << 32;
    clock_gettime(CLOCK_MONOTONIC, &now);
    value ^= (uint64_t) now.tv_sec << 32 | (uint32_t) now.tv_nsec;
    for (size_t index = 0; index != 8; ++index)
        output[index] = (uint8_t)(value >> (56 - index * 8));
}
