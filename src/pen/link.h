#ifndef LINK_H
#define LINK_H

#include "cfg.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>

void link_init(struct link *link);

void link_destroy(struct link *link);

int link_open(struct link *link, const struct cfg *cfg);

void link_close(struct link *link);

int link_video_open(struct video *video, const struct cfg *cfg);

void link_video_close(struct video *video);

int link_handshake(struct link *link, const struct cfg *cfg, struct mode mode, uint8_t nonce[8]);

int link_hello(const struct video *video, const struct cfg *cfg);

int link_send(struct link *link, const void *data, size_t length);

void link_start(struct link *link);

void link_stop(struct link *link);

bool link_running(const struct link *link);

#endif