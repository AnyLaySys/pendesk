#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>

#define STREAM_PATH "/tmp/pendesk-video.sock"

struct stream_packet {
    uint32_t length;
    uint32_t keyframe;
    uint64_t timestamp;
};

#endif