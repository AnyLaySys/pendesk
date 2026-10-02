#ifndef PREVIEW_H
#define PREVIEW_H

#include "cfg.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

struct preview_frame {
    uint8_t *data;
    size_t capacity;
    size_t length;
    uint64_t timestamp;
    bool keyframe;
};

struct files;
struct video_frame;
struct preview {
    pthread_mutex_t mutex;
    struct preview_frame queue[4];
    unsigned int head;
    unsigned int count;
    bool connected;
    bool waiting;
    atomic_bool keyframe;
    int wake;
    int listener;
    int video_listener;
    struct files *files;
    pthread_t thread;
    bool thread_started;
};

int preview_open(struct preview *preview, const struct cfg *cfg);

void preview_close(struct preview *preview);

void preview_publish(struct preview *preview, struct video_frame *frame);

void preview_reset(struct preview *preview);

#endif
