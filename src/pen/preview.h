#ifndef PREVIEW_H
#define PREVIEW_H

#include "cfg.h"
#include "state.h"

int preview_open(struct preview *preview, const struct cfg *cfg);

void preview_close(struct preview *preview);

int preview_publish(struct preview *preview, const uint8_t *frame, size_t length);

void preview_clear(struct preview *preview);

#endif