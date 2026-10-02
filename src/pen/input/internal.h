#ifndef PENDESK_INPUT_INTERNAL_H
#define PENDESK_INPUT_INTERNAL_H

#include "input.h"

int control_open(const char *path);

int control_read(struct input_state *input);

int devices_open(struct inputs *inputs);

int input_send_key(struct input_state *input, uint16_t key, bool down);

int keyboard_read(struct input_state *input, int fd);

int cancel_touch(struct input_state *input);

int touch_flush(struct input_state *input);

int touch_read(struct input_state *input);

#endif