#ifndef PENDESK_MEDIA_RECEIVE_H
#define PENDESK_MEDIA_RECEIVE_H

#include "cfg.h"
#include "input.h"
#include "link.h"
#include "preview.h"

struct audio;

int media_receive(struct input_state *input, struct preview *preview, struct video *video,
                  const struct cfg *cfg, struct audio *audio, bool configured);

#endif