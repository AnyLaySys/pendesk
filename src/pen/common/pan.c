#define _POSIX_C_SOURCE 200809L

#include "pan.h"
#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct pan_shared *pan_open(int create) {
    int fd = open(PAN_PATH, O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return NULL;
    struct stat info;
    if ((create && ftruncate(fd, sizeof(struct pan_shared)) != 0) || fstat(fd, &info) != 0 ||
        info.st_size != sizeof(struct pan_shared)) {
        close(fd);
        return NULL;
    }
    struct pan_shared *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                     0);
    close(fd);
    if (shared == MAP_FAILED) return NULL;
    if (create) {
        atomic_store(&shared->region, 0);
        atomic_store(&shared->rendered, 0);
        atomic_store(&shared->display, 0);
    }
    return shared;
}

void pan_close(struct pan_shared *shared) {
    if (shared) munmap(shared, sizeof(*shared));
}