#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "video.h"
#include "audio.h"
#include "input.h"
#include "io.h"
#include "link.h"
#include "preview.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint32_t) now.tv_nsec / 1000000;
}

static uint16_t read_u16(const uint8_t *source) {
    return (uint16_t)((uint16_t) source[0] << 8 | source[1]);
}

static uint32_t read_u32(const uint8_t *source) {
    return (uint32_t) source[0] << 24 | (uint32_t) source[1] << 16 | (uint32_t) source[2] << 8 |
           source[3];
}

static bool newer(uint32_t candidate, uint32_t previous) {
    return candidate != previous && candidate - previous < UINT32_C(0x80000000);
}

static int
start_frame(struct video *video, uint32_t sequence, uint32_t length, uint16_t fragments) {
    uint8_t *frame;
    uint8_t *received;
    if (video->capacity < length) {
        frame = realloc(video->frame, length);
        if (!frame) return -1;
        video->frame = frame;
        video->capacity = length;
    }
    if (video->fragments_capacity < fragments) {
        received = realloc(video->fragments, fragments);
        if (!received) return -1;
        video->fragments = received;
        video->fragments_capacity = fragments;
    }
    memset(video->fragments, 0, fragments);
    video->sequence = sequence;
    video->length = length;
    video->fragment_count = fragments;
    video->fragments_received = 0;
    video->assembling = true;
    video->has_sequence = true;
    video->fresh = false;
    return 0;
}

static int
receive_packet(struct video *video, const struct cfg *cfg, const struct input_state *input,
               struct audio *audio, const uint8_t *packet, size_t length, bool configured) {
    struct in_addr host;
    const uint8_t *payload;
    uint32_t sequence;
    uint32_t frame_length;
    uint16_t fragment;
    uint16_t fragments;
    size_t payload_length;
    size_t fragment_length;
    size_t offset;
    if (length < 10 + AUDIO_HEADER || packet[0] || packet[1] || packet[2] || packet[3] != 1 ||
        inet_pton(AF_INET, cfg->host, &host) != 1 || memcmp(packet + 4, &host, 4) ||
        read_u16(packet + 8) != cfg->port)
        return 0;
    payload = packet + 10;
    payload_length = length - 10;
    if (payload[4] != PROTOCOL_VERSION || memcmp(payload + 5, video->nonce, sizeof(video->nonce)))
        return 0;
    if (!memcmp(payload, "PDSA", 4)) {
        if (configured && !input_paused(input) && payload_length > AUDIO_HEADER)
            audio_play(audio, payload + AUDIO_HEADER, payload_length - AUDIO_HEADER);
        return 0;
    }
    if (memcmp(payload, "PDSV", 4) || payload_length < VIDEO_HEADER) return 0;
    sequence = read_u32(payload + 13);
    fragment = read_u16(payload + 17);
    fragments = read_u16(payload + 19);
    frame_length = read_u32(payload + 21);
    if (!frame_length || frame_length > MAX_FRAME || !fragments || fragment >= fragments ||
        fragments != (frame_length + VIDEO_PAYLOAD - 1) / VIDEO_PAYLOAD)
        return 0;
    offset = (size_t) fragment * VIDEO_PAYLOAD;
    fragment_length = frame_length - offset;
    if (fragment_length > VIDEO_PAYLOAD) fragment_length = VIDEO_PAYLOAD;
    if (payload_length != VIDEO_HEADER + fragment_length) return 0;
    video->connected = true;
    if (!video->has_sequence || newer(sequence, video->sequence)) {
        if (start_frame(video, sequence, frame_length, fragments) != 0) return -1;
    } else if (sequence != video->sequence || !video->assembling || video->length != frame_length ||
               video->fragment_count != fragments)
        return 0;
    if (!video->fragments[fragment]) {
        memcpy(video->frame + offset, payload + VIDEO_HEADER, fragment_length);
        video->fragments[fragment] = 1;
        ++video->fragments_received;
    }
    if (video->fragments_received != video->fragment_count) return 0;
    video->assembling = false;
    video->fresh = true;
    return 0;
}

static int
receive_packets(struct video *video, const struct cfg *cfg, const struct input_state *input,
                struct preview *preview, struct audio *audio, bool configured) {
    uint8_t packet[10 + VIDEO_PACKET];
    for (;;) {
        ssize_t length = recv(video->fd, packet, sizeof(packet), 0);
        if (length < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            break;
        }
        if (receive_packet(video, cfg, input, audio, packet, (size_t) length, configured) != 0)
            return -1;
    }
    if (video->fresh) {
        video->fresh = false;
        if (configured && !input_paused(input)) return preview_publish(preview, video->frame,
                                                                       video->length);
    }
    return 0;
}

static int apply_control(struct input_state *input, struct audio *audio, const uint8_t packet[10]) {
    struct view view;
    if (packet[0] == 0x11 && packet[1] == 5) {
        audio_start(audio, read_u32(packet + 2), packet[6]);
        return 1;
    }
    if (packet[0] != 0x10 || packet[1] != 2) return -1;
    view.x = read_u16(packet + 2);
    view.y = read_u16(packet + 4);
    view.width = read_u16(packet + 6);
    view.height = read_u16(packet + 8);
    if (!view.width || !view.height || (uint32_t) view.x + view.width > input->video.width ||
        (uint32_t) view.y + view.height > input->video.height)
        return -1;
    input_set_view(input, view);
    return 0;
}

void video_receive(struct input_state *input, struct preview *preview, struct video *video,
                   const struct cfg *cfg) {
    struct link *link = input->link;
    struct audio audio;
    uint8_t control[128];
    size_t control_length = 0;
    uint64_t next_hello = 0;
    bool configured = false;
    audio_init(&audio);
    while (alive && link_running(link)) {
        struct pollfd events[] = {{.fd = link->fd, .events = POLLIN},
                                  {.fd = video->fd, .events = POLLIN}};
        uint64_t now = milliseconds();
        int result;
        if (!video->connected && now >= next_hello) {
            link_hello(video, cfg);
            next_hello = now + 100;
        }
        result = poll(events, sizeof(events) / sizeof(events[0]), 100);
        if (result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (events[1].revents & POLLIN &&
            receive_packets(video, cfg, input, preview, &audio, configured) != 0)
            break;
        if (events[0].revents & POLLIN) {
            while (control_length < sizeof(control)) {
                ssize_t length = read(link->fd, control + control_length,
                                      sizeof(control) - control_length);
                if (length > 0) {
                    control_length += (size_t) length;
                    while (control_length >= 10) {
                        int applied = apply_control(input, &audio, control);
                        if (applied < 0) goto done;
                        if (applied == 0) configured = true;
                        memmove(control, control + 10, control_length - 10);
                        control_length -= 10;
                    }
                    continue;
                }
                if (!length || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    goto done;
                break;
            }
            if (control_length == sizeof(control)) break;
        }
        if (events[0].revents & (POLLERR | POLLHUP | POLLNVAL) ||
            events[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
    }
    done:
    audio_stop(&audio);
    preview_clear(preview);
}
