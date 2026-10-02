#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal.h"
#include "link.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int control_open(const char *path) {
    int fd;
    unlink(path);
    if (mkfifo(path, 0600) != 0) return -1;
    fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || chmod(path, 0600) != 0) {
        if (fd >= 0) close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

int control_read(struct input_state *input) {
    uint8_t packet[64];
    ssize_t count;
    while ((count = read(input->control, packet, sizeof(packet))) > 0) {
        if ((size_t) count > sizeof(input->control_buffer) - input->control_length) return -1;
        memcpy(input->control_buffer + input->control_length, packet, (size_t) count);
        input->control_length += (size_t) count;
        size_t offset = 0;
        while (offset < input->control_length) {
            uint8_t type = input->control_buffer[offset];
            size_t length = type == 2 || type == 0x27 || type == 0x28 || type == 0x29 ? 2 :
                            type == 0x23 ? 4 : 0;
            if (!length) return -1;
            if (offset + length > input->control_length) break;
            if (type == 2) {
                bool paused = input->control_buffer[offset + 1] & STATE_VIDEO_PAUSED;
                bool recording = atomic_load(&input->camera) ||
                                 (input->control_buffer[offset + 1] & STATE_RECORDING);
                atomic_store(&input->paused, paused);
                atomic_store(&input->recording, recording);
                input->blocked = input->control_buffer[offset + 1] & STATE_BLOCKED;
                input->mouse = !(input->control_buffer[offset + 1] & STATE_MOUSE_PAUSED);
                input->direct = input->control_buffer[offset + 1] & STATE_TOUCH;
                input->pad = input->control_buffer[offset + 1] & STATE_TOUCHPAD;
                if (cancel_touch(input) != 0) return -1;
                if (link_running(input->link)) {
                    uint8_t video[] = {0x25, paused ? 0 : 1};
                    if (link_send(input->link, video, sizeof(video)) != 0) return -1;
                }
            } else if (type == 0x28) {
                bool recording =
                        input->control_buffer[offset + 1] != 0 || atomic_load(&input->camera);
                atomic_store(&input->recording, recording);
            } else if (type == 0x29) {
                bool camera = input->control_buffer[offset + 1] != 0;
                atomic_store(&input->camera, camera);
                atomic_store(&input->recording, camera);
                if (link_send(input->link, input->control_buffer + offset, length) != 0) return -1;
            } else if (link_send(input->link, input->control_buffer + offset, length) != 0)
                return -1;
            offset += length;
        }
        if (offset) {
            memmove(input->control_buffer, input->control_buffer + offset,
                    input->control_length - offset);
            input->control_length -= offset;
        }
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}
