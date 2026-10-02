#ifndef PAN_H
#define PAN_H

#include <stdatomic.h>
#include <stdint.h>

#define PAN_PATH "/tmp/pendesk-view"

struct pan_shared {
    _Atomic uint64_t region;
    _Atomic uint32_t rendered;
    _Atomic uint32_t display;
    _Atomic uint32_t rotated;
};

struct pan_shared *pan_open(int create);

void pan_close(struct pan_shared *shared);

#endif
