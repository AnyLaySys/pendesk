#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal.h"
#include "link.h"
#include <errno.h>
#include <linux/input.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

enum {
    TAP_HOLD_MS = 300
};

static uint16_t scale(int value, int minimum, int maximum) {
    int64_t range;
    int64_t scaled;
    if (maximum <= minimum) return 0;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    range = (int64_t) maximum - minimum;
    scaled = ((int64_t) value - minimum) * UINT16_MAX / range;
    return (uint16_t) scaled;
}

void input_set_view(struct input_state *input, struct view view) {
    atomic_store(&input->view,
                 (uint64_t) view.x << 48 | (uint64_t) view.y << 32 | (uint64_t) view.width << 16 |
                 view.height);
}

static bool map_touch(const struct input_state *input, const struct contact *contact, uint16_t *x,
                      uint16_t *y) {
    const struct touch *touch = &input->inputs.touch;
    uint64_t packed = atomic_load(&input->view);
    struct view view = {.height = (uint16_t) packed, .width = (uint16_t)(
            packed >> 16), .x = (uint16_t)(packed >> 48), .y = (uint16_t)(packed >> 32)};
    uint32_t point_x;
    uint32_t point_y;
    if (!view.width || !view.height)
        return false;
    point_x = (uint32_t) scale(contact->x, touch->x_min, touch->x_max) * (input->mode.width - 1) /
              UINT16_MAX;
    point_y = (uint32_t) scale(contact->y, touch->y_min, touch->y_max) * (input->mode.height - 1) /
              UINT16_MAX;
    if (!input->mouse || input->blocked || (touch->gesture == GESTURE_IDLE &&
                                            (point_x < 6 || point_x + 6 >= input->mode.width ||
                                             (point_x + 44 >= input->mode.width && point_y < 60))))
        return false;
    if (point_x < view.x || point_y < view.y || point_x >= (uint32_t) view.x + view.width ||
        point_y >= (uint32_t) view.y + view.height)
        return false;
    *x = view.width > 1 ? (uint16_t)((point_x - view.x) * UINT16_MAX / (view.width - 1)) : 0;
    *y = view.height > 1 ? (uint16_t)((point_y - view.y) * UINT16_MAX / (view.height - 1)) : 0;
    return true;
}

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint32_t) now.tv_nsec / 1000000;
}

static int16_t clamp_delta(int32_t value) {
    return value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : (int16_t) value;
}

static int input_send_button(struct input_state *input, uint8_t button, bool down) {
    uint8_t packet[] = {0x21, button, down};
    return link_send(input->link, packet, sizeof(packet));
}


int input_send_key(struct input_state *input, uint16_t key, bool down) {
    uint8_t packet[] = {0x23, (uint8_t)(key >> 8), (uint8_t) key, down};
    return link_send(input->link, packet, sizeof(packet));
}

static int input_send_delta(struct input_state *input, uint8_t type, int16_t value) {
    uint16_t delta = (uint16_t) value;
    uint8_t packet[] = {type, (uint8_t)(delta >> 8), (uint8_t) delta};
    return link_send(input->link, packet, sizeof(packet));
}

int cancel_touch(struct input_state *input) {
    bool dragging = input->inputs.touch.gesture == GESTURE_DRAG;
    input->inputs.touch.gesture = GESTURE_CANCEL;
    return dragging ? input_send_button(input, 1, false) : 0;
}

static uint32_t
touch_distance(const struct input_state *input, uint16_t x0, uint16_t y0, uint16_t x1,
               uint16_t y1) {
    uint32_t x = (uint32_t) abs((int) x1 - x0) * input->mode.width / UINT16_MAX;
    uint32_t y = (uint32_t) abs((int) y1 - y0) * input->mode.height / UINT16_MAX;
    return x > y ? x + y / 2 : y + x / 2;
}

int touch_flush(struct input_state *input) {
    struct touch *touch = &input->inputs.touch;
    int first = -1;
    int second = -1;
    uint16_t x;
    uint16_t y;
    uint16_t other_x;
    uint16_t other_y;
    if (!input->mouse) return cancel_touch(input);
    uint64_t now = milliseconds();
    for (int index = 0; index != CONTACTS; ++index) {
        if (!touch->contacts[index].active) continue;
        if (first < 0) first = index;
        else {
            second = index;
            break;
        }
    }
    if (first < 0) {
        enum gesture gesture = touch->gesture;
        touch->gesture = GESTURE_IDLE;
        if (gesture == GESTURE_DRAG) return input_send_button(input, 1, false);
        uint8_t button = gesture == GESTURE_TAP ? 1 :
                         (gesture == GESTURE_PAIR || gesture == GESTURE_PAIR_END) &&
                         now - touch->started < TAP_HOLD_MS ? 2 : 0;
        if (button) {
            uint8_t packet[] = {0x21, button, 1, 0x21, button, 0};
            return link_send(input->link, packet, sizeof(packet));
        }
        return 0;
    }
    if (touch->gesture == GESTURE_CANCEL || touch->gesture == GESTURE_PAIR_END) return 0;
    if (!map_touch(input, &touch->contacts[first], &x, &y)) return cancel_touch(input);
    if (second >= 0) {
        uint32_t distance;
        int32_t delta;
        if (!map_touch(input, &touch->contacts[second], &other_x, &other_y))
            return cancel_touch(input);
        distance = touch_distance(input, x, y, other_x, other_y);
        x = (uint16_t)(((uint32_t) x + other_x) / 2);
        y = (uint16_t)(((uint32_t) y + other_y) / 2);
        if (touch->gesture != GESTURE_PAIR && touch->gesture != GESTURE_SCROLL &&
            touch->gesture != GESTURE_ZOOM) {
            if (cancel_touch(input) != 0) return -1;
            touch->gesture = GESTURE_PAIR;
            touch->started = now;
            touch->origin_x = touch->last_x = x;
            touch->origin_y = touch->last_y = y;
            touch->distance = distance;
            return 0;
        }
        if (touch->gesture == GESTURE_PAIR) {
            uint32_t spread = (uint32_t) abs((int32_t) distance - (int32_t) touch->distance);
            uint32_t motion = touch_distance(input, x, y, touch->origin_x, touch->origin_y);
            if (spread >= motion + GESTURE_THRESHOLD) touch->gesture = GESTURE_ZOOM;
            else if (motion >= spread + GESTURE_THRESHOLD) touch->gesture = GESTURE_SCROLL;
            else return 0;
        }
        if (touch->gesture == GESTURE_ZOOM) {
            delta = touch->distance ? ((int32_t) distance - (int32_t) touch->distance) * 160 /
                                      (int32_t) touch->distance : 0;
            if (delta) touch->distance = distance;
        } else {
            delta = (int32_t)(
                    (int64_t)((int32_t) x - touch->last_x) * input->mode.width * 3 / UINT16_MAX);
            if (delta) touch->last_x = x;
        }
        return delta ? input_send_delta(input, touch->gesture == GESTURE_ZOOM ? 0x24 : 0x26,
                                        clamp_delta(delta)) : 0;
    }
    if (touch->gesture == GESTURE_PAIR || touch->gesture == GESTURE_SCROLL ||
        touch->gesture == GESTURE_ZOOM) {
        touch->gesture = touch->gesture == GESTURE_PAIR ? GESTURE_PAIR_END : GESTURE_CANCEL;
        return 0;
    }
    if (touch->gesture == GESTURE_IDLE) {
        touch->gesture = GESTURE_TAP;
        touch->origin_x = touch->last_x = x;
        touch->origin_y = touch->last_y = y;
        touch->started = now;
        return input_send_move(input, 0, 0);
    }
    if (touch->gesture == GESTURE_TAP) {
        if (touch_distance(input, x, y, touch->origin_x, touch->origin_y) > 3)
            touch->gesture = GESTURE_MOVE;
        else if (now - touch->started >= TAP_HOLD_MS) {
            touch->gesture = GESTURE_DRAG;
            return input_send_button(input, 1, true);
        } else return 0;
    }
    int32_t dx = (int32_t) x - touch->last_x;
    int32_t dy = (int32_t) y - touch->last_y;
    touch->last_x = x;
    touch->last_y = y;
    return dx || dy ? input_send_move(input, clamp_delta(dx), clamp_delta(dy)) : 0;
}

static int touch_event(struct input_state *input, const struct input_event *event) {
    struct touch *touch = &input->inputs.touch;
    int slot = touch->multitouch ? touch->slot : 0;
    if (event->type == EV_ABS) {
        if (touch->multitouch && event->code == ABS_MT_SLOT) touch->slot = event->value;
        else if (event->code == ABS_MT_TRACKING_ID && slot >= 0 && slot < CONTACTS)
            touch->contacts[slot].active = event->value >= 0;
        else if (event->code == touch->x_code && slot >= 0 && slot < CONTACTS)
            touch->contacts[slot].x = event->value;
        else if (event->code == touch->y_code && slot >= 0 && slot < CONTACTS)
            touch->contacts[slot].y = event->value;
    } else if (!touch->multitouch && event->type == EV_KEY && event->code == BTN_TOUCH)
        touch->contacts[0].active = event->value != 0;
    else if (event->type == EV_SYN) {
        if (event->code == SYN_REPORT) return touch_flush(input);
        if (event->code == SYN_DROPPED) {
            for (size_t index = 0; index < CONTACTS; ++index) touch->contacts[index].active = false;
            return cancel_touch(input);
        }
    }
    return 0;
}

int touch_read(struct input_state *input) {
    struct input_event events[32];
    ssize_t count;
    while ((count = read(input->inputs.touch.fd, events, sizeof(events))) > 0) {
        if ((size_t) count % sizeof(events[0])) return -1;
        for (size_t index = 0; index != (size_t) count / sizeof(events[0]); ++index)
            if (touch_event(input, &events[index]) != 0) return -1;
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}