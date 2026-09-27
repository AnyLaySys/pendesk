#ifndef CAM_H
#define CAM_H

#include "cfg.h"
#include "state.h"

struct camera {
    int fd;
    pid_t process;
    char path[160];
    uint8_t *frame;
    size_t length;
    size_t capacity;
    uint32_t sequence;
    bool collecting;
    bool previous_ff;
};

void cam_init(struct camera *camera);
int cam_start(struct camera *camera, const struct cfg *cfg);
int cam_running(struct camera *camera);
void cam_stop(struct camera *camera);
int cam_forward(struct camera *camera, const struct video *video, const struct cfg *cfg);

#endif
