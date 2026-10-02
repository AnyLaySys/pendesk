#ifndef PENDESK_INPUT_INTERNAL_H
#define PENDESK_INPUT_INTERNAL_H

#include "input.h"

int input_control_open(const char *path);

int input_control_read(struct input_state *input);

int input_devices_open(struct inputs *inputs);

int input_send_key(struct input_state *input, uint16_t key, bool down);

int input_keyboard_read(struct input_state *input, int fd);

int input_cancel_touch(struct input_state *input);

int input_touch_flush(struct input_state *input);

int input_touch_read(struct input_state *input);

#endif