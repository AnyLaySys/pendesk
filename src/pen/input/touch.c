#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal.h"
#include "link.h"
#include "bytes.h"
#include "io.h"
#include <errno.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
    TAP_HOLD_MS = 300
};

static uint32_t scale(int value, int minimum, int maximum, uint32_t size) {
    int64_t range;
    int64_t scaled;
    if (maximum <= minimum || !size) return 0;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    range = (int64_t) maximum - minimum;
    scaled = ((int64_t) value - minimum) * (size - 1) / range;
    return (uint32_t) scaled;
}

void input_set_view(struct input_state *input, struct view view) {
    atomic_store(&input->view,
                 (uint64_t) view.x << 48 | (uint64_t) view.y << 32 | (uint64_t) view.width << 16 |
                 view.height);
}

static bool map_touch(const struct input_state *input, const struct contact *contact, uint16_t *x,
                      uint16_t *y) {
    const struct touch *touch = &input->inputs.touch;
    uint32_t point_x = scale(contact->x, touch->x_min, touch->x_max, input->mode.width);
    uint32_t point_y = scale(contact->y, touch->y_min, touch->y_max, input->mode.height);
    if (input->pad) {
        *x = touch->rotated ? input->mode.width - 1 - point_x : point_x;
        *y = touch->rotated ? input->mode.height - 1 - point_y : point_y;
        return true;
    }
    uint64_t packed = atomic_load(&input->view);
    struct view view = {.height = (uint16_t) packed, .width = (uint16_t)(
            packed >> 16), .x = (uint16_t)(packed >> 48), .y = (uint16_t)(packed >> 32)};
    if (!view.width || !view.height)
        return false;
    if (touch->gesture == GESTURE_IDLE &&
        (point_x < 6 || point_x + 6 >= input->mode.width ||
         (point_x + 44 >= input->mode.width && point_y < 60)))
        return false;
    if (point_x < view.x || point_y < view.y || point_x >= (uint32_t) view.x + view.width ||
        point_y >= (uint32_t) view.y + view.height)
        return false;
    *x = view.width > 1 ? (uint16_t)((point_x - view.x) * UINT16_MAX / (view.width - 1)) : 0;
    *y = view.height > 1 ? (uint16_t)((point_y - view.y) * UINT16_MAX / (view.height - 1)) : 0;
    return true;
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
    struct touch *touch = &input->inputs.touch;
    bool dragging = touch->gesture == GESTURE_DRAG;
    touch->gesture = GESTURE_CANCEL;
    touch->pad_contacts = 0;
    uint8_t packet[2 + CONTACTS * PAN_POINT_SIZE] = {0x2a, 0};
    for (unsigned int i = 0; i < CONTACTS; ++i) {
        touch->contacts[i].ignored = touch->contacts[i].active;
        if (!touch->sent[i].active) continue;
        uint8_t *point = packet + 2 + packet[1]++ * PAN_POINT_SIZE;
        point[0] = i;
        point[1] = 4;
        write_u16(point + 2, touch->sent[i].x);
        write_u16(point + 4, touch->sent[i].y);
    }
    memset(touch->sent, 0, sizeof(touch->sent));
    touch->pending_valid = false;
    if (packet[1] && link_send(input->link, packet, 2 + packet[1] * PAN_POINT_SIZE) != 0)
        return -1;
    return dragging ? input_send_button(input, 1, false) : 0;
}

static int direct_flush(struct input_state *input) {
    struct touch *touch = &input->inputs.touch;
    if (!touch->pending_valid) return 0;
    bool changed = false;
    uint8_t packet[2 + CONTACTS * PAN_POINT_SIZE] = {0x2a, 0};
    for (unsigned int i = 0; i < CONTACTS; ++i) {
        struct touch_point before = touch->sent[i], next = touch->pending[i];
        if (before.active != next.active ||
            (next.active && (before.x != next.x || before.y != next.y))) changed = true;
        if (!before.active && !next.active) continue;
        uint8_t *point = packet + 2 + packet[1]++ * PAN_POINT_SIZE;
        point[0] = i;
        point[1] = next.active ? (before.active ? 2 : 1) : 3;
        write_u16(point + 2, next.active ? next.x : before.x);
        write_u16(point + 4, next.active ? next.y : before.y);
    }
    if (changed && link_send(input->link, packet, 2 + packet[1] * PAN_POINT_SIZE) != 0)
        return -1;
    memcpy(touch->sent, touch->pending, sizeof(touch->sent));
    touch->pending_valid = false;
    return 0;
}

static int direct_report(struct input_state *input) {
    struct touch *touch = &input->inputs.touch;
    if (input->blocked || input_paused(input)) return cancel_touch(input);
    bool rotated = input->pan && atomic_load(&input->pan->rotated);
    if (touch->rotated != rotated) {
        touch->rotated = rotated;
        for (unsigned int i = 0; i < CONTACTS; ++i)
            if (touch->sent[i].active || (touch->pending_valid && touch->pending[i].active))
                return cancel_touch(input);
    }
    uint64_t packed = atomic_load(&input->view);
    struct view view = {.height = packed, .width = packed >> 16,
        .x = packed >> 48, .y = packed >> 32};
    uint32_t origin = input->pan ? atomic_load(&input->pan->rendered) : 0;
    struct touch_point next[CONTACTS] = {0};
    bool transition = false;
    for (unsigned int i = 0; i < CONTACTS; ++i) {
        struct contact *contact = &touch->contacts[i];
        if (!contact->active) contact->ignored = false;
        else if (!contact->ignored) {
            uint32_t x = scale(contact->x, touch->x_min, touch->x_max, input->mode.width);
            uint32_t y = scale(contact->y, touch->y_min, touch->y_max, input->mode.height);
            uint32_t screen_x = rotated ? y : input->mode.height - 1 - y;
            uint32_t screen_y = rotated ? input->mode.width - 1 - x : x;
            bool held = touch->sent[i].active || (touch->pending_valid && touch->pending[i].active);
            if (!view.width || !view.height || (!held &&
                (x < view.x || y < view.y || x >= (uint32_t)view.x + view.width ||
                 y >= (uint32_t)view.y + view.height || x < 6 || x + 6 >= input->mode.width ||
                 (screen_y + 44 >= input->mode.width && screen_x + 60 >= input->mode.height)))) contact->ignored = true;
            else next[i] = (struct touch_point) {
                .x = (origin >> 16) + screen_x,
                .y = (origin & 65535) + screen_y, .active = true};
        }
        if (next[i].active != (touch->pending_valid ? touch->pending[i].active : touch->sent[i].active))
            transition = true;
    }
    if (transition && direct_flush(input) != 0) return -1;
    memcpy(touch->pending, next, sizeof(next));
    touch->pending_valid = true;
    return transition ? direct_flush(input) : 0;
}

int touch_timeout(const struct input_state *input) {
    const struct touch *touch = &input->inputs.touch;
    if (input->direct || touch->gesture != GESTURE_TAP) return -1;
    uint64_t now = milliseconds();
    return now >= touch->started + TAP_HOLD_MS ? 0 : (int)(touch->started + TAP_HOLD_MS - now);
}

static uint32_t
touch_distance(const struct input_state *input, uint16_t x0, uint16_t y0, uint16_t x1,
               uint16_t y1) {
    uint32_t x = (uint32_t) abs((int) x1 - x0);
    uint32_t y = (uint32_t) abs((int) y1 - y0);
    if (!input->pad) {
        x = x * input->mode.width / UINT16_MAX;
        y = y * input->mode.height / UINT16_MAX;
    }
    return x > y ? x + y / 2 : y + x / 2;
}

int touch_flush(struct input_state *input) {
    struct touch *touch = &input->inputs.touch;
    if (input->direct) return direct_flush(input);
    int first = -1;
    int second = -1;
    uint16_t x;
    uint16_t y;
    uint16_t other_x;
    uint16_t other_y;
    if (!input->mouse || (input->blocked && !input->pad)) return cancel_touch(input);
    if (input->pad) {
        bool rotated = input->pan && atomic_load(&input->pan->rotated);
        if (touch->rotated != rotated) {
            touch->rotated = rotated;
            if (touch->pad_contacts) return cancel_touch(input);
        }
    }
    uint64_t now = milliseconds();
    for (int index = 0; index != CONTACTS; ++index) {
        struct contact *contact = &touch->contacts[index];
        if (!contact->active) continue;
        if (input->pad) {
            if (contact->ignored) continue;
            if (!(touch->pad_contacts & (1u << index))) {
                uint32_t y = scale(contact->y, touch->y_min, touch->y_max, input->mode.height);
                uint32_t screen_x = touch->rotated ? y : input->mode.height - 1 - y;
                if (screen_x * 100 < input->mode.height * 73u) {
                    contact->ignored = true;
                    continue;
                }
                touch->pad_contacts |= 1u << index;
            }
        }
        if (first < 0) first = index;
        else {
            second = index;
            break;
        }
    }
    if (input->pad && touch->gesture != GESTURE_IDLE && touch->gesture != GESTURE_CANCEL &&
        !(touch->pad_contacts & (1u << touch->primary))) first = -1;
    if (first < 0) {
        enum gesture gesture = touch->gesture;
        if (input->pad) {
            touch->pad_contacts = 0;
            for (unsigned int i = 0; i < CONTACTS; ++i)
                touch->contacts[i].ignored = touch->contacts[i].active;
        }
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
    if (touch->gesture == GESTURE_CANCEL ||
        (!input->pad && touch->gesture == GESTURE_PAIR_END)) return 0;
    if (input->pad && touch->gesture == GESTURE_IDLE) touch->primary = first;
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
            if (touch->gesture == GESTURE_DRAG && input_send_button(input, 1, false) != 0)
                return -1;
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
            delta = input->pad ? ((int32_t) x - touch->last_x) * 8 : (int32_t)(
                    (int64_t)((int32_t) x - touch->last_x) * input->mode.width * 3 / UINT16_MAX);
            if (delta) touch->last_x = x;
        }
        return delta ? input_send_delta(input, touch->gesture == GESTURE_ZOOM ? 0x24 : 0x26,
                                        clamp_delta(delta)) : 0;
    }
    if (touch->gesture == GESTURE_PAIR || touch->gesture == GESTURE_SCROLL ||
        touch->gesture == GESTURE_ZOOM) {
        touch->gesture = touch->gesture == GESTURE_PAIR ? GESTURE_PAIR_END :
                         input->pad ? GESTURE_MOVE : GESTURE_CANCEL;
        if (input->pad) {
            touch->origin_x = touch->last_x = x;
            touch->origin_y = touch->last_y = y;
        }
        return 0;
    }
    if (touch->gesture == GESTURE_IDLE) {
        touch->gesture = GESTURE_TAP;
        touch->origin_x = touch->last_x = x;
        touch->origin_y = touch->last_y = y;
        touch->started = now;
        return input_send_move(input, 0, 0);
    }
    if (touch->gesture == GESTURE_TAP || (input->pad && touch->gesture == GESTURE_PAIR_END)) {
        if (touch_distance(input, x, y, touch->origin_x, touch->origin_y) > 3)
            touch->gesture = GESTURE_MOVE;
        else if (touch->gesture == GESTURE_TAP && now - touch->started >= TAP_HOLD_MS) {
            touch->gesture = GESTURE_DRAG;
            return input_send_button(input, 1, true);
        } else if (!input->pad) return 0;
    }
    int32_t dx = (int32_t) x - touch->last_x;
    int32_t dy = (int32_t) y - touch->last_y;
    touch->last_x = x;
    touch->last_y = y;
    if (input->pad) {
        dx *= 256;
        dy *= 70;
    }
    return dx || dy ? input_send_move(input, clamp_delta(dx), clamp_delta(dy)) : 0;
}

static int touch_event(struct input_state *input, const struct input_event *event) {
    struct touch *touch = &input->inputs.touch;
    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        touch->dropped = true;
        return cancel_touch(input);
    }
    if (touch->dropped) {
        if (event->type != EV_SYN || event->code != SYN_REPORT) return 0;
        touch->dropped = false;
        if (touch->multitouch) {
            int ids[CONTACTS + 1] = {ABS_MT_TRACKING_ID};
            int x[CONTACTS + 1] = {ABS_MT_POSITION_X};
            int y[CONTACTS + 1] = {ABS_MT_POSITION_Y};
            struct input_absinfo slot;
            if (ioctl(touch->fd, EVIOCGMTSLOTS(sizeof(ids)), ids) != 0 ||
                ioctl(touch->fd, EVIOCGMTSLOTS(sizeof(x)), x) != 0 ||
                ioctl(touch->fd, EVIOCGMTSLOTS(sizeof(y)), y) != 0 ||
                ioctl(touch->fd, EVIOCGABS(ABS_MT_SLOT), &slot) != 0) return -1;
            touch->slot = slot.value;
            for (unsigned int i = 0; i < CONTACTS; ++i)
                touch->contacts[i] = (struct contact) {.x = x[i + 1], .y = y[i + 1],
                    .tracking = ids[i + 1], .active = ids[i + 1] >= 0, .ignored = true};
        } else {
            touch->contacts[0].active = false;
            touch->contacts[0].ignored = true;
        }
        return 0;
    }
    int slot = touch->multitouch ? touch->slot : 0;
    if (event->type == EV_ABS) {
        if (touch->multitouch && event->code == ABS_MT_SLOT) touch->slot = event->value;
        else if (event->code == ABS_MT_TRACKING_ID && slot >= 0 && slot < CONTACTS) {
            struct contact *contact = &touch->contacts[slot];
            if ((input->direct || input->pad) && contact->active && event->value >= 0 &&
                contact->tracking != event->value) {
                contact->active = false;
                touch->pad_contacts &= ~(1u << slot);
                if ((input->direct ? direct_report(input) : touch_flush(input)) != 0) return -1;
            }
            touch->pad_contacts &= ~(1u << slot);
            if (!contact->active && event->value >= 0) contact->ignored = false;
            contact->active = event->value >= 0;
            contact->tracking = event->value;
        }
        else if (event->code == touch->x_code && slot >= 0 && slot < CONTACTS)
            touch->contacts[slot].x = event->value;
        else if (event->code == touch->y_code && slot >= 0 && slot < CONTACTS)
            touch->contacts[slot].y = event->value;
    } else if (!touch->multitouch && event->type == EV_KEY && event->code == BTN_TOUCH) {
        touch->pad_contacts = 0;
        if (!touch->contacts[0].active && event->value) touch->contacts[0].ignored = false;
        touch->contacts[0].active = event->value != 0;
    } else if (event->type == EV_SYN) {
        if (event->code == SYN_REPORT)
            return input->direct ? direct_report(input) : touch_flush(input);
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
