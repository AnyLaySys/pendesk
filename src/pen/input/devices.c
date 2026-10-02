#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal.h"
#include "keymap.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool bit(const unsigned long *bits, unsigned int code) {
    return bits[code / BIT_WORD] & 1UL << (code % BIT_WORD);
}

static int add_keyboard(struct inputs *inputs, int fd) {
    int *keyboard = realloc(inputs->keyboard, (inputs->keyboard_count + 1) * sizeof(*keyboard));
    if (!keyboard) return -1;
    inputs->keyboard = keyboard;
    inputs->keyboard[inputs->keyboard_count++] = fd;
    return 0;
}

int devices_open(struct inputs *inputs) {
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

int keyboard_read(struct input_state *input, int fd) {
    struct input_event events[32];
    ssize_t count;
    while ((count = read(fd, events, sizeof(events))) > 0) {
        if ((size_t) count % sizeof(events[0])) return -1;
        for (size_t index = 0; index != (size_t) count / sizeof(events[0]); ++index) {
            uint16_t key;
            if (events[index].type == EV_KEY && events[index].value <= 2 &&
                (key = virtual_key(events[index].code)) &&
                input_send_key(input, key, events[index].value != 0) != 0)
                return -1;
        }
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}