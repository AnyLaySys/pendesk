#ifndef VIDEO_FRAMES_H
#define VIDEO_FRAMES_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct video_frame {
    uint8_t *data;
    uint8_t *parity;
    uint8_t *received;
    size_t capacity;
    size_t length;
    uint64_t timestamp;
    uint64_t started;
    uint32_t sequence;
    uint16_t fragments;
    uint16_t count;
    bool active;
    bool keyframe;
};

struct video_frames {
    struct video_frame slots[4];
    uint32_t next;
    bool initialized;
    bool needs_keyframe;
};

int
video_frames_push(struct video_frames *frames, const uint8_t *packet, size_t length, uint64_t now);

struct video_frame *video_frames_next(struct video_frames *frames, uint64_t now);

void video_frames_free(struct video_frames *frames);

#endif