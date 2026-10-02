#include "frames.h"
#include "bytes.h"
#include <stdlib.h>
#include <string.h>

static bool keyframe(const struct video_frame *frame) {
    for (size_t i = 0; i + 3 < frame->length; ++i)
        if (!frame->data[i] && !frame->data[i + 1] && frame->data[i + 2] == 1 &&
            (frame->data[i + 3] & 31) == 5)
            return true;
    return false;
}

int
frames_push(struct video_frames *frames, const uint8_t *packet, size_t length, uint64_t now) {
    if (length < VIDEO_HEADER) return 0;
    bool parity = !memcmp(packet, "PDSF", 4);
    if (!parity && memcmp(packet, "PDSV", 4)) return 0;
    uint32_t sequence = read_u32(packet + 13), size = read_u32(packet + 21);
    uint16_t index = read_u16(packet + 17), count = read_u16(packet + 19);
    if (!size || size > MAX_FRAME || !count || index >= count ||
        count != (size + VIDEO_PAYLOAD - 1) / VIDEO_PAYLOAD)
        return 0;
    size_t offset = (size_t) index * VIDEO_PAYLOAD;
    size_t payload = size - offset;
    if (payload > VIDEO_PAYLOAD) payload = VIDEO_PAYLOAD;
    if (length != VIDEO_HEADER + payload || (parity && index % VIDEO_GROUP)) return 0;
    if (!frames->initialized) {
        frames->initialized = frames->needs_keyframe = true;
        frames->next = sequence;
    }
    uint32_t distance = sequence - frames->next;
    if (distance >= UINT32_C(0x80000000)) return 0;
    if (distance >= 4) {
        frames->next = sequence - 3;
        frames->needs_keyframe = true;
    }
    struct video_frame *frame = &frames->slots[sequence % 4];
    size_t groups = (count + VIDEO_GROUP - 1) / VIDEO_GROUP;
    if (!frame->active || frame->sequence != sequence) {
        size_t capacity = (size_t) count * VIDEO_PAYLOAD + groups * VIDEO_PAYLOAD + count + groups;
        if (frame->capacity < capacity) {
            uint8_t *replacement = realloc(frame->data, capacity);
            if (!replacement) return -1;
            frame->data = replacement;
            frame->capacity = capacity;
        }
        frame->parity = frame->data + (size_t) count * VIDEO_PAYLOAD;
        frame->received = frame->parity + groups * VIDEO_PAYLOAD;
        memset(frame->parity, 0, groups * VIDEO_PAYLOAD + count + groups);
        frame->sequence = sequence;
        frame->length = size;
        frame->fragments = count;
        frame->count = 0;
        frame->started = now;
        frame->timestamp = 0;
        for (unsigned int i = 25; i < 33; ++i) frame->timestamp = frame->timestamp << 8 | packet[i];
        frame->active = true;
    }
    if (frame->length != size || frame->fragments != count) return 0;
    unsigned int group = index / VIDEO_GROUP;
    size_t marker = parity ? count + group : index;
    if (frame->received[marker]) return 0;
    frame->received[marker] = 1;
    uint8_t *accumulator = frame->parity + group * VIDEO_PAYLOAD;
    for (size_t i = 0; i < payload; ++i) accumulator[i] ^= packet[VIDEO_HEADER + i];
    if (!parity) {
        memcpy(frame->data + offset, packet + VIDEO_HEADER, payload);
        ++frame->count;
    }
    if (frame->received[count + group]) {
        unsigned int missing = 0, absent = 0;
        unsigned int last = (group + 1) * VIDEO_GROUP;
        if (last > count) last = count;
        for (unsigned int i = group * VIDEO_GROUP; i < last; ++i)
            if (!frame->received[i]) {
                ++missing;
                absent = i;
            }
        if (missing == 1) {
            size_t amount = size - (size_t) absent * VIDEO_PAYLOAD;
            if (amount > VIDEO_PAYLOAD) amount = VIDEO_PAYLOAD;
            memcpy(frame->data + (size_t) absent * VIDEO_PAYLOAD, accumulator, amount);
            frame->received[absent] = 1;
            ++frame->count;
        }
    }
    return 0;
}

struct video_frame *frames_next(struct video_frames *frames, uint64_t now) {
    if (!frames->initialized) return NULL;
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
        struct video_frame *frame = &frames->slots[frames->next % 4];
        bool present = frame->active && frame->sequence == frames->next;
        if (present && frame->count == frame->fragments) {
            frame->active = false;
            ++frames->next;
            frame->keyframe = keyframe(frame);
            if (frame->keyframe) frames->needs_keyframe = false;
            if (!frames->needs_keyframe) return frame;
            continue;
        }
        uint64_t oldest = present ? frame->started : UINT64_MAX;
        if (!present) {
            for (unsigned int i = 0; i < 4; ++i) {
                struct video_frame *queued = &frames->slots[i];
                if (queued->active && queued->sequence - frames->next < 4 &&
                    queued->started < oldest)
                    oldest = queued->started;
            }
        }
        if (oldest == UINT64_MAX || now - oldest < 30) return NULL;
        if (present) frame->active = false;
        ++frames->next;
        frames->needs_keyframe = true;
    }
    return NULL;
}

void frames_free(struct video_frames *frames) {
    for (unsigned int i = 0; i < 4; ++i) free(frames->slots[i].data);
    memset(frames, 0, sizeof(*frames));
}