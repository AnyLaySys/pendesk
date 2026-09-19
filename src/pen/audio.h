#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

struct audio {
    int input;
    int process;
};

void audio_init(struct audio *audio);
int audio_start(struct audio *audio, uint32_t rate, uint8_t channels);
void audio_play(struct audio *audio, const uint8_t *samples, size_t length);
void audio_stop(struct audio *audio);

#endif
