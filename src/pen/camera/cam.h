#ifndef CAM_H
#define CAM_H

#include "cfg.h"
#include "link.h"
#include "stream.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
    CAMERA_MAX_FRAME = 8 * 1024 * 1024
};

struct camera_frame {
    uint8_t *data;
    size_t capacity;
    size_t length;
    uint64_t timestamp;
};

struct camera {
    int fd;
    pid_t process;
    char path[160];
    struct camera_frame incoming, pending, sending;
    struct stream_packet header;
    size_t received;
    uint32_t sequence;
    uint16_t fragment;
    bool parity_pending;
    uint8_t parity[VIDEO_PAYLOAD];
    uint64_t next_send;
};

void cam_init(struct camera *camera);

int cam_start(struct camera *camera);

int cam_running(struct camera *camera);

void cam_stop(struct camera *camera);

int cam_forward(struct camera *camera, const struct video *video, const struct cfg *cfg);

int cam_timeout(const struct camera *camera, uint64_t now);

#endif
