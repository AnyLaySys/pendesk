#ifndef INPUT_H
#define INPUT_H

#include "protocol.h"
#include "pan.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

enum {
    BIT_WORD = sizeof(unsigned long) * 8,
    CONTACTS = 2,
    GESTURE_THRESHOLD = 9,
    MOVE_HISTORY = 128,
    PAN_POINT_SIZE = 6
};

struct link;
struct contact {
    int x;
    int y;
    int tracking;
    bool active;
    bool ignored;
};
struct touch_point {
    uint16_t x, y;
    bool active;
};
struct mode {
    uint16_t width;
    uint16_t height;
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
    unsigned int pad_contacts;
    int primary;
    uint16_t last_x;
    uint16_t last_y;
    uint16_t origin_x;
    uint16_t origin_y;
    uint32_t distance;
    uint64_t started;
    struct contact contacts[CONTACTS];
    struct touch_point sent[CONTACTS];
    struct touch_point pending[CONTACTS];
    bool pending_valid;
    bool dropped;
    bool rotated;
};
struct inputs {
    int *keyboard;
    size_t keyboard_count;
    struct touch touch;
};
struct input_state {
    struct pan_shared *pan;
    pthread_mutex_t pan_mutex;
    uint16_t canvas_width;
    uint16_t canvas_height;
    float pointer_x;
    float pointer_y;
    float pan_x;
    float pan_y;
    uint32_t move_sequence;
    struct {
        uint32_t sequence;
        int16_t x, y;
    } moves[MOVE_HISTORY];
    struct link *link;
    struct inputs inputs;
    struct mode mode;
    atomic_uint_fast64_t view;
    atomic_bool paused;
    atomic_bool recording;
    atomic_bool camera;
    int control;
    bool blocked;
    bool mouse;
    bool direct;
    bool pad;
    pthread_t thread;
    bool thread_started;
    char control_path[108];
    uint8_t control_buffer[128];
    size_t control_length;
};

int input_open(struct input_state *input, const char *config);

void input_close(struct input_state *input);

int input_start(struct input_state *input, struct link *link, struct mode mode);

void input_stop(struct input_state *input);

void input_set_view(struct input_state *input, struct view view);

bool input_paused(const struct input_state *input);

bool input_recording(const struct input_state *input);

bool input_camera(const struct input_state *input);

void input_pan_configure(struct input_state *input, uint16_t width, uint16_t height, uint16_t x,
                         uint16_t y);

void input_pan_ack(struct input_state *input, uint16_t x, uint16_t y, uint32_t sequence);

int input_send_move(struct input_state *input, int16_t x, int16_t y);

#endif
