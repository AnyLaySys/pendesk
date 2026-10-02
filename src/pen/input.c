#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "input_internal.h"
#include "cfg.h"
#include "link.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

static void *input_loop(void *argument) {
    struct input_state *input = argument;
    size_t count = input->inputs.keyboard_count + 2;
    struct pollfd *events = calloc(count, sizeof(*events));
    if (!events) {
        link_stop(input->link);
        return NULL;
    }
    events[0] = (struct pollfd) {.fd = input->control, .events = POLLIN};
    events[1] = (struct pollfd) {.fd = input->inputs.touch.fd, .events = POLLIN};
    for (size_t index = 0; index != input->inputs.keyboard_count; ++index)
        events[index + 2] = (struct pollfd) {.fd = input->inputs.keyboard[index], .events = POLLIN};
    while (link_running(input->link)) {
        int result = poll(events, count, 20);
        if (result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (events[0].revents && input_control_read(input) != 0) break;
        if (events[1].revents && input_touch_read(input) != 0) break;
        for (size_t index = 0; index != input->inputs.keyboard_count; ++index) {
            if (events[index + 2].revents &&
                input_keyboard_read(input, events[index + 2].fd) != 0) {
                result = -1;
                break;
            }
        }
        if (result < 0 || input_touch_flush(input) != 0) break;
    }
    free(events);
    link_stop(input->link);
    return NULL;
}

int input_open(struct input_state *input, const char *config) {
    *input = (struct input_state) {.blocked = true, .control = -1, .pan_mutex = PTHREAD_MUTEX_INITIALIZER};
    input->pan = pan_open(1);
    if (!input->pan || input_devices_open(&input->inputs) != 0 ||
        cfg_control_path(input->control_path, sizeof(input->control_path), config) != 0) {
        input_close(input);
        return -1;
    }
    if ((input->control = input_control_open(input->control_path)) < 0) {
        input_close(input);
        return -1;
    }
    atomic_init(&input->view, 0);
    atomic_init(&input->paused, true);
    atomic_init(&input->recording, false);
    atomic_init(&input->camera, false);
    return 0;
}

bool input_camera(const struct input_state *input) {
    return atomic_load(&input->camera);
}

void input_stop(struct input_state *input) {
    if (!input->thread_started) return;
    link_stop(input->link);
    pthread_join(input->thread, NULL);
    input->thread_started = false;
}

void input_close(struct input_state *input) {
    input_stop(input);
    if (input->control >= 0) close(input->control);
    if (input->control_path[0]) unlink(input->control_path);
    if (input->inputs.touch.fd >= 0) close(input->inputs.touch.fd);
    for (size_t index = 0; index < input->inputs.keyboard_count; ++index)
        close(input->inputs.keyboard[index]);
    free(input->inputs.keyboard);
    pan_close(input->pan);
    pthread_mutex_destroy(&input->pan_mutex);
    *input = (struct input_state) {.control = -1};
}

int input_start(struct input_state *input, struct link *link, struct mode mode) {
    input->link = link;
    input->mode = mode;
    if (input->pan) atomic_store(&input->pan->display, (uint32_t) mode.height << 16 | mode.width);
    input->move_sequence = 0;
    input->inputs.touch.gesture = GESTURE_CANCEL;
    input_set_view(input, (struct view) {0});
    return pthread_create(&input->thread, NULL, input_loop, input) == 0
           ? (input->thread_started = true, 0) : -1;
}

bool input_paused(const struct input_state *input) {
    return atomic_load(&input->paused);
}

bool input_recording(const struct input_state *input) {
    return atomic_load(&input->recording);
}