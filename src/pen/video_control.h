#ifndef PENDESK_VIDEO_CONTROL_H
#define PENDESK_VIDEO_CONTROL_H

#include <stdint.h>

struct input_state;
struct audio;

int video_control(struct input_state *input, struct audio *audio, const uint8_t packet[10]);

#endif
