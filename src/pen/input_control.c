#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "input_internal.h"
#include "link.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

int input_control_open(const char *path) {
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

int input_control_read(struct input_state *input) {
    uint8_t packet[64];
    ssize_t count;
    while ((count = recv(input->control, packet, sizeof(packet), 0)) > 0) {
        size_t offset = 0;
        while (offset < (size_t) count) {
            uint8_t type = packet[offset];
            size_t length =
                    type == 2 || type == 0x27 ? 2 : type == 0x20 ? 5 : type == 0x21 || type == 0x26
                                                                       ? 3 : type == 0x23 ? 4 :
                                                                             type == 0x28 ||
                                                                             type == 0x29 ? 2 : 0;
            if (!length || offset + length > (size_t) count) return 0;
            if (type == 2) {
                bool paused = packet[offset + 1] & STATE_VIDEO_PAUSED;
                bool recording =
                        atomic_load(&input->camera) || (packet[offset + 1] & STATE_RECORDING);
                atomic_store(&input->paused, paused);
                atomic_store(&input->recording, recording);
                input->blocked = packet[offset + 1] & STATE_BLOCKED;
                input->mouse = !(packet[offset + 1] & STATE_MOUSE_PAUSED);
                if (input_cancel_touch(input) != 0) return -1;
                if (link_running(input->link)) {
                    uint8_t video[] = {0x25, paused ? 0 : 1};
                    if (link_send(input->link, video, sizeof(video)) != 0) return -1;
                }
            } else if (type == 0x28) {
                bool recording = packet[offset + 1] != 0 || atomic_load(&input->camera);
                atomic_store(&input->recording, recording);
            } else if (type == 0x29) {
                bool camera = packet[offset + 1] != 0;
                atomic_store(&input->camera, camera);
                atomic_store(&input->recording, camera);
                if (link_send(input->link, packet + offset, length) != 0) return -1;
            } else if ((input->mouse || (type != 0x20 && type != 0x21 && type != 0x26)) &&
                       link_send(input->link, packet + offset, length) != 0)
                return -1;
            offset += length;
        }
    }
    return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}