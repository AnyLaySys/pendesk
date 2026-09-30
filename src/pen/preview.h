#ifndef PREVIEW_H
#define PREVIEW_H

#include "cfg.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct files;
struct preview {
    pthread_mutex_t mutex;
    uint8_t *frame;
    size_t capacity;
    size_t length;
    uint64_t sequence;
    int listener;
    struct files *files;
    pthread_t thread;
    bool thread_started;
};

int preview_open(struct preview *preview, const struct cfg *cfg);

void preview_close(struct preview *preview);

int preview_publish(struct preview *preview, const uint8_t *frame, size_t length);

#endif
