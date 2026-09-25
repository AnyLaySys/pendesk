#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "input.h"
#include "cfg.h"
#include "link.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

int input_send_control(const char *path, const char *text) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    uint8_t packet[64];
    size_t length = strlen(text);
    int fd;
    if (!length || length > sizeof(packet) * 2 || length % 2 ||
        strlen(path) >= sizeof(address.sun_path))
        return 1;
    for (size_t index = 0; index < length / 2; ++index) {
        int high = nibble(text[index * 2]);
        int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return 1;
        packet[index] = (uint8_t)(high * 16 + low);
    }
    strcpy(address.sun_path, path);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return 1;
    ssize_t sent = sendto(fd, packet, length / 2, 0, (struct sockaddr *) &address, sizeof(address));
    close(fd);
    return sent == (ssize_t)(length / 2) ? 0 : 1;
}

static int open_control(const char *path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    int fd;
    if (strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    unlink(path);
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0 || chmod(path, 0600) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int input_display_mode(struct mode *mode) {
    DIR *directory = opendir("/sys/class/drm");
    struct dirent *entry;
    if (!directory) return -1;
    while ((entry = readdir(directory))) {
        char status[256];
        char modes[256];
        char value[64];
        FILE *file;
        unsigned int width;
        unsigned int height;
        if (strncmp(entry->d_name, "card", 4) || !strchr(entry->d_name, '-') ||
            strstr(entry->d_name, "Writeback") ||
            snprintf(status, sizeof(status), "/sys/class/drm/%s/status", entry->d_name) >=
            (int) sizeof(status) ||
            snprintf(modes, sizeof(modes), "/sys/class/drm/%s/modes", entry->d_name) >=
            (int) sizeof(modes))
            continue;
        file = fopen(status, "r");
        if (!file) continue;
        if (!fgets(value, sizeof(value), file) || strcmp(value, "connected\n")) {
            fclose(file);
            continue;
        }
        fclose(file);
        file = fopen(modes, "r");
        if (!file) continue;
        if (fscanf(file, "%ux%u", &width, &height) == 2 && width && height && width <= UINT16_MAX &&
            height <= UINT16_MAX) {
            fclose(file);
            closedir(directory);
            mode->width = (uint16_t) width;
            mode->height = (uint16_t) height;
            return 0;
        }
        fclose(file);
    }
    closedir(directory);
    return -1;
}

static bool bit(const unsigned long *bits, unsigned int code) {
    return bits[code / BIT_WORD] & 1UL << (code % BIT_WORD);
}

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
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t point_x;
    uint32_t point_y;
    if (!view.width || !view.height || input->video.width > input->mode.width ||
        input->video.height > input->mode.height)
        return false;
    offset_x = (input->mode.width - input->video.width) / 2;
    offset_y = (input->mode.height - input->video.height) / 2;
    point_x = (uint32_t) scale(contact->x, touch->x_min, touch->x_max) * (input->mode.width - 1) /
              UINT16_MAX;
    point_y = (uint32_t) scale(contact->y, touch->y_min, touch->y_max) * (input->mode.height - 1) /
              UINT16_MAX;
    if (!input->mouse || input->blocked || (touch->gesture == GESTURE_IDLE &&
                           (point_x < 6 || point_x + 6 >= input->mode.width ||
                            (point_x + 44 >= input->mode.width && point_y < 60))))
        return false;
    if (point_x < offset_x || point_y < offset_y || point_x >= offset_x + input->video.width ||
        point_y >= offset_y + input->video.height)
        return false;
    point_x -= offset_x;
    point_y -= offset_y;
    if (point_x < view.x || point_y < view.y || point_x >= (uint32_t) view.x + view.width ||
        point_y >= (uint32_t) view.y + view.height)
        return false;
    *x = view.width > 1 ? (uint16_t)((point_x - view.x) * UINT16_MAX / (view.width - 1)) : 0;
    *y = view.height > 1 ? (uint16_t)((point_y - view.y) * UINT16_MAX / (view.height - 1)) : 0;
    return true;
}

static int add_keyboard(struct inputs *inputs, int fd) {
    int *keyboard = realloc(inputs->keyboard, (inputs->keyboard_count + 1) * sizeof(*keyboard));
    if (!keyboard) return -1;
    inputs->keyboard = keyboard;
    inputs->keyboard[inputs->keyboard_count++] = fd;
    return 0;
}

static int open_inputs(struct inputs *inputs) {
    DIR *directory;
    struct dirent *entry;
    memset(inputs, 0, sizeof(*inputs));
    inputs->touch.fd = -1;
    directory = opendir("/dev/input");
    if (!directory) return -1;
    while ((entry = readdir(directory))) {
        unsigned long types[(EV_MAX + BIT_WORD) / BIT_WORD] = {0};
        char path[128];
        int fd;
        if (strncmp(entry->d_name, "event", 5) || entry->d_name[5] == '\0' ||
            snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name) >= (int) sizeof(path))
            continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 || ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) < 0) {
            if (fd >= 0) close(fd);
            continue;
        }
        if (inputs->touch.fd < 0 && bit(types, EV_ABS)) {
            unsigned long axes[(ABS_MAX + BIT_WORD) / BIT_WORD] = {0};
            struct input_absinfo slot;
            struct input_absinfo x;
            struct input_absinfo y;
            unsigned short x_code = ABS_MT_POSITION_X;
            unsigned short y_code = ABS_MT_POSITION_Y;
            bool multi;
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(axes)), axes) < 0) {
                close(fd);
                continue;
            }
            multi = bit(axes, ABS_MT_SLOT) && ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0 &&
                    slot.maximum > 0;
            if (!bit(axes, x_code) || !bit(axes, y_code) || ioctl(fd, EVIOCGABS(x_code), &x) != 0 ||
                ioctl(fd, EVIOCGABS(y_code), &y) != 0) {
                unsigned long keys[(KEY_MAX + BIT_WORD) / BIT_WORD] = {0};
                if (!bit(types, EV_KEY) || ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
                    !bit(keys, BTN_TOUCH) || ioctl(fd, EVIOCGABS(ABS_X), &x) != 0 ||
                    ioctl(fd, EVIOCGABS(ABS_Y), &y) != 0) {
                    close(fd);
                    continue;
                }
                x_code = ABS_X;
                y_code = ABS_Y;
                multi = false;
            }
            if (x.maximum <= x.minimum || y.maximum <= y.minimum) {
                close(fd);
                continue;
            }
            inputs->touch = (struct touch) {.fd = fd, .x_max = x.maximum, .x_min = x.minimum, .y_max = y.maximum, .y_min = y.minimum, .x_code = x_code, .y_code = y_code, .multitouch = multi, .slot = multi
                                                                                                                                                                                                       ? slot.value
                                                                                                                                                                                                       : 0};
            if (multi) {
                int positions_x[CONTACTS + 1] = {ABS_MT_POSITION_X};
                int positions_y[CONTACTS + 1] = {ABS_MT_POSITION_Y};
                if (ioctl(fd, EVIOCGMTSLOTS(sizeof(positions_x)), positions_x) != 0 ||
                    ioctl(fd, EVIOCGMTSLOTS(sizeof(positions_y)), positions_y) != 0) {
                    close(fd);
                    inputs->touch.fd = -1;
                    continue;
                }
                for (size_t index = 0; index < CONTACTS; ++index) {
                    inputs->touch.contacts[index].x = positions_x[index + 1];
                    inputs->touch.contacts[index].y = positions_y[index + 1];
                }
            } else {
                inputs->touch.contacts[0].x = x.value;
                inputs->touch.contacts[0].y = y.value;
            }
            continue;
        }
        if (bit(types, EV_KEY)) {
            unsigned long keys[(KEY_MAX + BIT_WORD) / BIT_WORD] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && bit(keys, KEY_A) &&
                bit(keys, KEY_Z) && bit(keys, KEY_SPACE) && add_keyboard(inputs, fd) == 0)
                continue;
        }
        close(fd);
    }
    closedir(directory);
    if (inputs->touch.fd >= 0) return 0;
    for (size_t index = 0; index < inputs->keyboard_count; ++index) close(inputs->keyboard[index]);
    free(inputs->keyboard);
    return -1;
}

static uint16_t virtual_key(unsigned int code) {
    if (code >= KEY_A && code <= KEY_Z) return (uint16_t)(0x41 + code - KEY_A);
    if (code >= KEY_1 && code <= KEY_9) return (uint16_t)(0x31 + code - KEY_1);
    if (code >= KEY_F1 && code <= KEY_F12) return (uint16_t)(0x70 + code - KEY_F1);
    switch (code) {
        case KEY_0:
            return 0x30;
        case KEY_MINUS:
            return 0xbd;
        case KEY_EQUAL:
            return 0xbb;
        case KEY_LEFTBRACE:
            return 0xdb;
        case KEY_RIGHTBRACE:
            return 0xdd;
        case KEY_BACKSLASH:
            return 0xdc;
        case KEY_SEMICOLON:
            return 0xba;
        case KEY_APOSTROPHE:
            return 0xde;
        case KEY_GRAVE:
            return 0xc0;
        case KEY_COMMA:
            return 0xbc;
        case KEY_DOT:
            return 0xbe;
        case KEY_SLASH:
            return 0xbf;
        case KEY_BACKSPACE:
            return 0x08;
        case KEY_TAB:
            return 0x09;
        case KEY_ENTER:
            return 0x0d;
        case KEY_LEFTCTRL:
            return 0xa2;
        case KEY_LEFTSHIFT:
            return 0xa0;
        case KEY_LEFTALT:
            return 0xa4;
        case KEY_CAPSLOCK:
            return 0x14;
        case KEY_ESC:
            return 0x1b;
        case KEY_SPACE:
            return 0x20;
        case KEY_PAGEUP:
            return 0x21;
        case KEY_PAGEDOWN:
            return 0x22;
        case KEY_END:
            return 0x23;
        case KEY_HOME:
            return 0x24;
        case KEY_LEFT:
            return 0x25;
        case KEY_UP:
            return 0x26;
        case KEY_RIGHT:
            return 0x27;
        case KEY_DOWN:
            return 0x28;
        case KEY_INSERT:
            return 0x2d;
        case KEY_DELETE:
            return 0x2e;
        case KEY_RIGHTCTRL:
            return 0xa3;
        case KEY_RIGHTSHIFT:
            return 0xa1;
        case KEY_RIGHTALT:
            return 0xa5;
        case KEY_LEFTMETA:
            return 0x5b;
        case KEY_RIGHTMETA:
            return 0x5c;
        case KEY_MENU:
            return 0x5d;
        case KEY_KP0:
            return 0x60;
        case KEY_KP1:
            return 0x61;
        case KEY_KP2:
            return 0x62;
        case KEY_KP3:
            return 0x63;
        case KEY_KP4:
            return 0x64;
        case KEY_KP5:
            return 0x65;
        case KEY_KP6:
            return 0x66;
        case KEY_KP7:
            return 0x67;
        case KEY_KP8:
            return 0x68;
        case KEY_KP9:
            return 0x69;
        case KEY_KPASTERISK:
            return 0x6a;
        case KEY_KPPLUS:
            return 0x6b;
        case KEY_KPMINUS:
            return 0x6d;
        case KEY_KPDOT:
            return 0x6e;
        case KEY_KPSLASH:
            return 0x6f;
        case KEY_KPENTER:
            return 0x0d;
        default:
            return 0;
    }
}

static int16_t clamp_delta(int32_t value) {
    return value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : (int16_t) value;
}

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint32_t) now.tv_nsec / 1000000;
}

static int send_button(struct input_state *input, uint8_t button, bool down) {
    uint8_t packet[] = {0x21, button, down};
    return link_send(input->link, packet, sizeof(packet));
}

static int send_move(struct input_state *input, int16_t x, int16_t y) {
    uint16_t dx = (uint16_t) x;
    uint16_t dy = (uint16_t) y;
    uint8_t packet[] = {0x20, (uint8_t)(dx >> 8), (uint8_t) dx, (uint8_t)(dy >> 8), (uint8_t) dy};
    return link_send(input->link, packet, sizeof(packet));
}

static int send_key(struct input_state *input, uint16_t key, bool down) {
    uint8_t packet[] = {0x23, (uint8_t)(key >> 8), (uint8_t) key, down};
    return link_send(input->link, packet, sizeof(packet));
}

static int send_delta(struct input_state *input, uint8_t type, int16_t value) {
    uint16_t delta = (uint16_t) value;
    uint8_t packet[] = {type, (uint8_t)(delta >> 8), (uint8_t) delta};
    return link_send(input->link, packet, sizeof(packet));
}

static int cancel_touch(struct input_state *input) {
    bool dragging = input->inputs.touch.gesture == GESTURE_DRAG;
    input->inputs.touch.gesture = GESTURE_CANCEL;
    return dragging ? send_button(input, 1, false) : 0;
}

static int read_control(struct input_state *input) {
    uint8_t packet[64];
    ssize_t count;
    while ((count = recv(input->control, packet, sizeof(packet), 0)) > 0) {
        size_t offset = 0;
        while (offset < (size_t) count) {
            uint8_t type = packet[offset];
            size_t length =
                    type == 2 || type == 0x27 ? 2 : type == 0x20 ? 5 : type == 0x21 ||
                    type == 0x26 ? 3 : type == 0x23 ? 4 : type == 0x28 ? 2 : 0;
            if (!length || offset + length > (size_t) count) return 0;
            if (type == 2) {
                bool paused = packet[offset + 1] & STATE_VIDEO_PAUSED;
                bool recording = packet[offset + 1] & STATE_RECORDING;
                atomic_store(&input->paused, paused);
                atomic_store(&input->recording, recording);
                input->blocked = packet[offset + 1] & STATE_BLOCKED;
                input->mouse = !(packet[offset + 1] & STATE_MOUSE_PAUSED);
                if (cancel_touch(input) != 0) return -1;
                if (link_running(input->link)) {
                    uint8_t video[] = {0x25, paused ? 0 : 1};
                    if (link_send(input->link, video, sizeof(video)) != 0) return -1;
                }
            } else if (type == 0x28) {
                bool recording = packet[offset + 1] != 0;
                atomic_store(&input->recording, recording);
            } else if ((input->mouse || (type != 0x20 && type != 0x21 && type != 0x26)) &&
                     link_send(input->link, packet + offset, length) != 0) return -1;
            offset += length;
        }
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}

static uint32_t
touch_distance(const struct input_state *input, uint16_t x0, uint16_t y0, uint16_t x1,
               uint16_t y1) {
    uint32_t x = (uint32_t) abs((int) x1 - x0) * input->video.width / UINT16_MAX;
    uint32_t y = (uint32_t) abs((int) y1 - y0) * input->video.height / UINT16_MAX;
    return x > y ? x + y / 2 : y + x / 2;
}

static int flush_touch(struct input_state *input) {
    struct touch *touch = &input->inputs.touch;
    int first = -1;
    int second = -1;
    uint16_t x;
    uint16_t y;
    uint16_t other_x;
    uint16_t other_y;
    uint64_t now = milliseconds();
    if (!input->mouse) return cancel_touch(input);
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
        if (gesture == GESTURE_DRAG) return send_button(input, 1, false);
        uint8_t button = gesture == GESTURE_TAP ? 1 :
                         (gesture == GESTURE_PAIR || gesture == GESTURE_PAIR_END) &&
                         now - touch->started < 400 ? 2 : 0;
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
            if (spread >= 8 && spread > motion) touch->gesture = GESTURE_ZOOM;
            else if (motion >= 8 && motion > spread) touch->gesture = GESTURE_SCROLL;
            else return 0;
        }
        if (touch->gesture == GESTURE_ZOOM) {
            delta = touch->distance ? ((int32_t) distance - (int32_t) touch->distance) * 160 /
                                      (int32_t) touch->distance : 0;
            if (delta) touch->distance = distance;
        } else {
            delta = (int32_t)(
                    (int64_t)((int32_t) x - touch->last_x) * input->video.width * 3 / UINT16_MAX);
            if (delta) touch->last_x = x;
        }
        return delta ? send_delta(input, touch->gesture == GESTURE_ZOOM ? 0x24 : 0x26,
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
        return send_move(input, 0, 0);
    }
    if (touch->gesture == GESTURE_TAP) {
        if (touch_distance(input, x, y, touch->origin_x, touch->origin_y) >= 5)
            touch->gesture = GESTURE_MOVE;
        else if (now - touch->started >= 400) {
            touch->gesture = GESTURE_DRAG;
            return send_button(input, 1, true);
        } else return 0;
    }
    int32_t dx = (int32_t) x - touch->last_x;
    int32_t dy = (int32_t) y - touch->last_y;
    touch->last_x = x;
    touch->last_y = y;
    return dx || dy ? send_move(input, clamp_delta(dx), clamp_delta(dy)) : 0;
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
        if (event->code == SYN_REPORT) return flush_touch(input);
        if (event->code == SYN_DROPPED) {
            for (size_t index = 0; index < CONTACTS; ++index) touch->contacts[index].active = false;
            return cancel_touch(input);
        }
    }
    return 0;
}

static int read_touch(struct input_state *input) {
    struct input_event events[32];
    ssize_t count;
    while ((count = read(input->inputs.touch.fd, events, sizeof(events))) > 0) {
        if ((size_t) count % sizeof(events[0])) return -1;
        for (size_t index = 0; index != (size_t) count / sizeof(events[0]); ++index)
            if (touch_event(input, &events[index]) != 0)return -1;
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}

static int read_keyboard(struct input_state *input, int fd) {
    struct input_event events[32];
    ssize_t count;
    while ((count = read(fd, events, sizeof(events))) > 0) {
        if ((size_t) count % sizeof(events[0])) return -1;
        for (size_t index = 0; index != (size_t) count / sizeof(events[0]); ++index) {
            uint16_t key;
            if (events[index].type == EV_KEY && events[index].value <= 2 &&
                (key = virtual_key(events[index].code)) &&
                send_key(input, key, events[index].value != 0) != 0)
                return -1;
        }
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}

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
        if (events[0].revents && read_control(input) != 0) break;
        if (events[1].revents && read_touch(input) != 0) break;
        for (size_t index = 0; index != input->inputs.keyboard_count; ++index) {
            if (events[index + 2].revents && read_keyboard(input, events[index + 2].fd) != 0) {
                result = -1;
                break;
            }
        }
        if (result < 0 || flush_touch(input) != 0) break;
    }
    free(events);
    link_stop(input->link);
    return NULL;
}

int input_open(struct input_state *input, const char *config) {
    *input = (struct input_state) {.blocked = true, .mouse = true, .control = -1};
    if (open_inputs(&input->inputs) != 0 ||
        cfg_control_path(input->control_path, sizeof(input->control_path), config) != 0) {
        input_close(input);
        return -1;
    }
    if ((input->control = open_control(input->control_path)) < 0) {
        input_close(input);
        return -1;
    }
    atomic_init(&input->view, 0);
    atomic_init(&input->paused, false);
    atomic_init(&input->recording, false);
    return 0;
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
    *input = (struct input_state) {.control = -1};
}

int input_start(struct input_state *input, struct link *link, struct mode mode) {
    input->link = link;
    input->mode = mode;
    input->video = mode;
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
