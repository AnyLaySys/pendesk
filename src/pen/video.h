#ifndef VIDEO_H
#define VIDEO_H

#include "cfg.h"
#include "state.h"

void video_receive(struct input_state *input, struct preview *preview, struct video *video,
                   const struct cfg *cfg);

#endif