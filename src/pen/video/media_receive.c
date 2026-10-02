#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "media_receive.h"
#include "audio.h"
#include "bytes.h"
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

static int
receive_packet(struct video *video, const struct cfg *cfg, const struct input_state *input,
               struct audio *audio, const uint8_t *packet, size_t length, bool configured,
               uint64_t now) {
    struct in_addr host;
    const uint8_t *payload;
    if (length < 10 + VIDEO_ACK_HEADER || packet[0] || packet[1] || packet[2] || packet[3] != 1 ||
        inet_pton(AF_INET, cfg->host, &host) != 1 || memcmp(packet + 4, &host, 4) ||
        read_u16(packet + 8) != cfg->port)
        return 0;
    payload = packet + 10;
    size_t payload_length = length - 10;
    if (payload[4] != PROTOCOL_VERSION || memcmp(payload + 5, video->nonce, sizeof(video->nonce)))
        return 0;
    if (!memcmp(payload, "PDSH", 4)) {
        if (payload_length == VIDEO_ACK_HEADER) video->connected = true;
        return 0;
    }
    if (!memcmp(payload, "PDSA", 4)) {
        if (configured && !input_paused(input) && payload_length > AUDIO_HEADER)
            audio_play(audio, read_u32(payload + 13), payload + AUDIO_HEADER,
                       payload_length - AUDIO_HEADER);
        return 0;
    }
    if (payload_length < VIDEO_HEADER || (memcmp(payload, "PDSV", 4) && memcmp(payload, "PDSF", 4)))
        return 0;
    video->connected = true;
    return frames_push(&video->frames, payload, payload_length, now);
}

int media_receive(struct input_state *input, struct preview *preview, struct video *video,
                  const struct cfg *cfg, struct audio *audio, bool configured, uint64_t now) {
    uint8_t packet[10 + VIDEO_PACKET];
    for (;;) {
        ssize_t length = recv(video->fd, packet, sizeof(packet), 0);
        if (length < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        } else if (receive_packet(video, cfg, input, audio, packet, (size_t) length, configured,
                                  now) != 0)
            return -1;
        struct video_frame *frame;
        while ((frame = frames_next(&video->frames, now))) {
            if (configured && !input_paused(input)) preview_publish(preview, frame);
        }
        if (length < 0) break;
    }
    if (video->frames.needs_keyframe) atomic_store(&preview->keyframe, true);
    return 0;
}
