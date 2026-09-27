#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct audio {
    int socket;
    int process;
    struct sockaddr_in address;
};

void audio_init(struct audio *audio);

int audio_start(struct audio *audio);

void audio_play(struct audio *audio, uint32_t sequence, const uint8_t *frame, size_t length);

void audio_stop(struct audio *audio);

#endif
