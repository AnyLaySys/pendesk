#ifndef INPUT_H
#define INPUT_H

#include "state.h"

int input_send_control(const char *config, const char *text);

int input_open(struct input_state *input, const char *config);

void input_close(struct input_state *input);

int input_display_mode(struct mode *mode);

int input_start(struct input_state *input, struct link *link, struct mode mode);

void input_stop(struct input_state *input);

void input_set_view(struct input_state *input, struct view view);

bool input_paused(const struct input_state *input);

#endif