#ifndef STATE_H
#define STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BIT_WORD = sizeof(unsigned long) * 8,
    CONTACTS = 2,
    MAX_FRAME = 32 * 1024 * 1024,
    STATE_BLOCKED = 1,
    STATE_VIDEO_PAUSED = 2,
    PROTOCOL_VERSION = 7,
    VIDEO_HEADER = 25,
    VIDEO_PAYLOAD = 1150,
    VIDEO_PACKET = VIDEO_HEADER + VIDEO_PAYLOAD,
    AUDIO_HEADER = 17,
    AUDIO_PAYLOAD = 1152,
    AUDIO_PACKET = AUDIO_HEADER + AUDIO_PAYLOAD
};
struct contact {
    int x;
    int y;
    bool active;
};
struct mode {
    uint16_t width;
    uint16_t height;
};
struct link {
    int fd;
    atomic_bool running;
    pthread_mutex_t write_mutex;
};
struct video {
    int association;
    int fd;
    uint8_t nonce[8];
    uint8_t *frame;
    size_t capacity;
    uint8_t *fragments;
    size_t fragments_capacity;
    uint32_t sequence;
    uint32_t length;
    uint16_t fragment_count;
    uint16_t fragments_received;
    bool assembling;
    bool has_sequence;
    bool connected;
    bool fresh;
};
struct view {
    uint16_t height;
    uint16_t width;
    uint16_t x;
    uint16_t y;
};
enum gesture {
    GESTURE_IDLE,
    GESTURE_TAP,
    GESTURE_MOVE,
    GESTURE_DRAG,
    GESTURE_PAIR,
    GESTURE_SCROLL,
    GESTURE_ZOOM,
    GESTURE_PAIR_END,
    GESTURE_CANCEL
};
struct touch {
    int fd;
    int x_max;
    int x_min;
    int y_max;
    int y_min;
    unsigned short x_code;
    unsigned short y_code;
    int slot;
    bool multitouch;
    enum gesture gesture;
    uint16_t last_x;
    uint16_t last_y;
    uint16_t origin_x;
    uint16_t origin_y;
    uint32_t distance;
    uint64_t started;
    struct contact contacts[CONTACTS];
};
struct inputs {
    int *keyboard;
    size_t keyboard_count;
    struct touch touch;
};
struct input_state {
    struct link *link;
    struct inputs inputs;
    struct mode mode;
    struct mode video;
    atomic_uint_fast64_t view;
    atomic_bool paused;
    int control;
    bool blocked;
    pthread_t thread;
    bool thread_started;
    char control_path[108];
};
struct files;
struct preview {
    pthread_mutex_t mutex;
    uint8_t *frame;
    size_t capacity;
    size_t length;
    uint64_t sequence;
    int listener;
    int event[2];
    struct files *files;
    pthread_t thread;
    bool thread_started;
};
#endif
