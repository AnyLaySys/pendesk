#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cam.h"
#include "bytes.h"
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    CAMERA_HEADER = 25, CAMERA_PAYLOAD = 1150
};

static void
send_frame(const struct camera *camera, const struct video *video, const struct cfg *cfg) {
    uint8_t packet[10 + CAMERA_HEADER + CAMERA_PAYLOAD] = {0, 0, 0, 1};
    struct in_addr host;
    uint16_t port = htons(cfg->port);
    uint16_t fragments = (uint16_t)((camera->length + CAMERA_PAYLOAD - 1) / CAMERA_PAYLOAD);
    if (inet_pton(AF_INET, cfg->host, &host) != 1 || !fragments) return;
    memcpy(packet + 4, &host, sizeof(host));
    memcpy(packet + 8, &port, sizeof(port));
    memcpy(packet + 10, "PDSC", 4);
    packet[14] = PROTOCOL_VERSION;
    memcpy(packet + 15, video->nonce, sizeof(video->nonce));
    write_u32(packet + 23, camera->sequence);
    write_u16(packet + 29, fragments);
    write_u32(packet + 31, (uint32_t) camera->length);
    for (uint16_t fragment = 0; fragment < fragments; ++fragment) {
        size_t offset = (size_t) fragment * CAMERA_PAYLOAD;
        size_t length = camera->length - offset;
        if (length > CAMERA_PAYLOAD) length = CAMERA_PAYLOAD;
        write_u16(packet + 27, fragment);
        memcpy(packet + 10 + CAMERA_HEADER, camera->frame + offset, length);
        if (send(video->fd, packet, 10 + CAMERA_HEADER + length, 0) < 0) return;
        struct timespec delay = {.tv_nsec = 100000};
        nanosleep(&delay, NULL);
    }
}

static void
jpeg_byte(struct camera *camera, uint8_t value, const struct video *video, const struct cfg *cfg) {
    if (!camera->collecting) {
        if (camera->previous_ff && value == 0xd8) {
            camera->frame[0] = 0xff;
            camera->frame[1] = 0xd8;
            camera->length = 2;
            camera->collecting = true;
        }
        camera->previous_ff = value == 0xff;
        return;
    }
    if (camera->length == camera->capacity) {
        camera->collecting = false;
        camera->length = 0;
        camera->previous_ff = false;
        return;
    }
    camera->frame[camera->length++] = value;
    if (camera->previous_ff && value == 0xd9) {
        ++camera->sequence;
        send_frame(camera, video, cfg);
        camera->length = 0;
        camera->collecting = false;
    }
    camera->previous_ff = value == 0xff;
}

int cam_forward(struct camera *camera, const struct video *video, const struct cfg *cfg) {
    uint8_t bytes[8192];
    ssize_t length = read(camera->fd, bytes, sizeof(bytes));
    if (length < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (!length) return 0;
    for (ssize_t index = 0; index < length; ++index) jpeg_byte(camera, bytes[index], video, cfg);
    return 0;
}