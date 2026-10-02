#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "video.h"
#include "audio.h"
#include "cam.h"
#include "input.h"
#include "io.h"
#include "link.h"
#include "mic.h"
#include "preview.h"
#include "media_receive.h"
#include "control.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint32_t) now.tv_nsec / 1000000;
}

void video_receive(struct input_state *input, struct preview *preview, struct video *video,
                   const struct cfg *cfg) {
    struct link *link = input->link;
    struct audio audio;
    struct camera camera;
    struct microphone microphone;
    uint8_t control[128];
    size_t control_length = 0;
    uint64_t next_hello = 0;
    uint64_t next_keyframe = 0;
    preview_reset(preview);
    bool configured = false;
    bool microphone_announced = false;
    audio_init(&audio);
    cam_init(&camera);
    mic_init(&microphone, video->nonce);
    while (alive && link_running(link)) {
        if (camera.process > 0 && !cam_running(&camera)) {
            atomic_store(&input->camera, false);
            atomic_store(&input->recording, false);
        }
        if (input_camera(input) && camera.process <= 0 && cam_start(&camera, cfg) != 0) {
            atomic_store(&input->camera, false);
            atomic_store(&input->recording, false);
        }
        if (!input_camera(input) && camera.process > 0) cam_stop(&camera);
        if (input_recording(input)) {
            if (microphone.process <= 0) {
                if (mic_start(&microphone) == 0 && mic_state(video, cfg, true) == 0)
                    microphone_announced = true;
                else {
                    mic_stop(&microphone);
                    atomic_store(&input->recording, false);
                    if (input_camera(input)) {
                        atomic_store(&input->camera, false);
                        cam_stop(&camera);
                    }
                }
            }
        } else if (microphone.process > 0) {
            if (microphone_announced) mic_state(video, cfg, false);
            microphone_announced = false;
            mic_stop(&microphone);
        }
        if (microphone.process > 0 && !mic_running(&microphone)) {
            atomic_store(&input->recording, false);
            if (microphone_announced) mic_state(video, cfg, false);
            microphone_announced = false;
        }
        struct pollfd events[] = {{.fd = link->fd, .events = POLLIN},
                                  {.fd = video->fd, .events = POLLIN},
                                  {.fd = microphone.fd, .events = POLLIN},
                                  {.fd = camera.process > 0 ? camera.fd : -1, .events = POLLIN}};
        uint64_t now = milliseconds();
        int result;
        if (now >= next_keyframe && atomic_exchange(&preview->keyframe, false)) {
            const uint8_t request = 0x2b;
            if (link_send(link, &request, 1) != 0) break;
            next_keyframe = now + 100;
        }
        if (!video->connected && now >= next_hello) {
            link_hello(video, cfg);
            next_hello = now + 100;
        }
        result = poll(events, sizeof(events) / sizeof(events[0]), 5);
        if (result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (media_receive(input, preview, video, cfg, &audio, configured, milliseconds()) != 0)
            break;
        if (events[2].revents & POLLIN && mic_forward(&microphone, video, cfg) != 0) break;
        if (events[3].revents & POLLIN && cam_forward(&camera, video, cfg) != 0) break;
        if (events[0].revents & POLLIN) {
            while (control_length < sizeof(control)) {
                ssize_t length = read(link->fd, control + control_length,
                                      sizeof(control) - control_length);
                if (length > 0) {
                    control_length += (size_t) length;
                    while (control_length >= 10) {
                        int applied = video_control(input, &audio, control);
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
    if (microphone_announced) mic_state(video, cfg, false);
    mic_stop(&microphone);
    cam_stop(&camera);
    audio_stop(&audio);
}