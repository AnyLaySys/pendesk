#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cam.h"
#include "bytes.h"
#include "io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void swap(struct camera_frame *first, struct camera_frame *second) {
    struct camera_frame spare = *first;
    *first = *second;
    *second = spare;
}

static int receive_frame(struct camera *camera) {
    bool header = camera->received < sizeof(camera->header);
    uint8_t *target = header ? (uint8_t *)&camera->header : camera->incoming.data;
    size_t offset = header ? camera->received : camera->received - sizeof(camera->header);
    size_t length = header ? sizeof(camera->header) : camera->header.length;
    ssize_t count = read(camera->fd, target + offset, length - offset);
    if (count < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    if (!count) return 0;
    camera->received += (size_t)count;
    if (header && camera->received == sizeof(camera->header)) {
        if (!camera->header.length || camera->header.length > CAMERA_MAX_FRAME) return -1;
        if (camera->incoming.capacity < camera->header.length) {
            uint8_t *data = realloc(camera->incoming.data, camera->header.length);
            if (!data) return -1;
            camera->incoming.data = data;
            camera->incoming.capacity = camera->header.length;
        }
    } else if (!header && camera->received == sizeof(camera->header) + camera->header.length) {
        camera->incoming.length = camera->header.length;
        camera->incoming.timestamp = camera->header.timestamp;
        swap(&camera->incoming, &camera->pending);
        camera->received = 0;
    }
    return 0;
}

int cam_timeout(const struct camera *camera, uint64_t now) {
    if (!camera->sending.length && !camera->pending.length) return 5;
    if (now >= camera->next_send) return 0;
    uint64_t delay = camera->next_send - now;
    return delay < 5 ? (int)delay : 5;
}

int cam_forward(struct camera *camera, const struct video *video, const struct cfg *cfg) {
    if (receive_frame(camera) != 0) return -1;
    uint64_t now = milliseconds();
    if (now < camera->next_send) return 0;
    if (!camera->sending.length && camera->pending.length) {
        swap(&camera->sending, &camera->pending);
        camera->pending.length = 0;
        camera->fragment = 0;
        camera->parity_pending = false;
        memset(camera->parity, 0, sizeof(camera->parity));
        ++camera->sequence;
    }
    const struct camera_frame *frame = &camera->sending;
    if (!frame->length) return 0;
    uint8_t packet[10 + VIDEO_HEADER] = {0, 0, 0, 1};
    if (inet_pton(AF_INET, cfg->host, packet + 4) != 1) return -1;
    write_u16(packet + 8, cfg->port);
    packet[14] = PROTOCOL_VERSION;
    memcpy(packet + 15, video->nonce, sizeof(video->nonce));
    write_u32(packet + 23, camera->sequence);
    uint16_t fragments = (uint16_t)((frame->length + VIDEO_PAYLOAD - 1) / VIDEO_PAYLOAD);
    write_u16(packet + 29, fragments);
    write_u32(packet + 31, (uint32_t)frame->length);
    uint64_t stamp = frame->timestamp;
    write_u32(packet + 35, (uint32_t)(stamp >> 32));
    write_u32(packet + 39, (uint32_t)stamp);
    for (unsigned int budget = 0; budget < VIDEO_GROUP + 1; ++budget) {
        bool parity = camera->parity_pending;
        uint16_t index = parity ? (camera->fragment - 1) / VIDEO_GROUP * VIDEO_GROUP : camera->fragment;
        size_t offset = (size_t)index * VIDEO_PAYLOAD;
        size_t length = frame->length - offset;
        if (length > VIDEO_PAYLOAD) length = VIDEO_PAYLOAD;
        const uint8_t *payload = parity ? camera->parity : frame->data + offset;
        memcpy(packet + 10, parity ? "PDCP" : "PDSC", 4);
        write_u16(packet + 27, index);
        struct iovec buffers[] = {{packet, sizeof(packet)}, {(void *)payload, length}};
        struct msghdr message = {.msg_iov = buffers, .msg_iovlen = 2};
        ssize_t sent = sendmsg(video->fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) { --budget; continue; }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) break;
        if (sent != (ssize_t)(sizeof(packet) + length)) return -1;
        if (parity) {
            camera->parity_pending = false;
            memset(camera->parity, 0, sizeof(camera->parity));
            if (camera->fragment == fragments) { camera->sending.length = 0; break; }
        } else {
            for (size_t i = 0; i < length; ++i) camera->parity[i] ^= payload[i];
            ++camera->fragment;
            camera->parity_pending = camera->fragment % VIDEO_GROUP == 0 || camera->fragment == fragments;
        }
    }
    camera->next_send = now + 1;
    return 0;
}
