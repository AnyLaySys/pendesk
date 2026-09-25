#ifndef MIC_H
#define MIC_H

#include "cfg.h"
#include "state.h"

struct microphone {
    int fd;
    pid_t process;
    char path[128];
    uint8_t nonce[8];
};

void mic_init(struct microphone *microphone, const uint8_t nonce[8]);
int mic_start(struct microphone *microphone);
int mic_running(struct microphone *microphone);
void mic_stop(struct microphone *microphone);
int mic_state(const struct video *video, const struct cfg *cfg, bool recording);
int mic_forward(struct microphone *microphone, const struct video *video, const struct cfg *cfg);

#endif
