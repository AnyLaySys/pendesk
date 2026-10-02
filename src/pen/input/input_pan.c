#include "input.h"
#include "link.h"

static float clamp(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

static void follow(struct input_state *input) {
    if (!input->pan || !input->canvas_width || !input->canvas_height) return;
    float width = input->mode.height, height = input->mode.width;
    float x = input->pointer_x - input->pan_x, y = input->pointer_y - input->pan_y;
    input->pan_x += x - clamp(x, 24, width - 25);
    input->pan_y += y - clamp(y, 24, height - 25);
    input->pan_x = clamp(input->pan_x, 0,
                         input->canvas_width > width ? input->canvas_width - width : 0);
    input->pan_y = clamp(input->pan_y, 0,
                         input->canvas_height > height ? input->canvas_height - height : 0);
    atomic_store(&input->pan->region,
                 (uint64_t) input->canvas_width << 48 | (uint64_t) input->canvas_height << 32 |
                 (uint64_t)(uint16_t)(input->pan_x * 16) << 16 | (uint16_t)(input->pan_y * 16));
}

static void move(struct input_state *input, int16_t x, int16_t y) {
    if (!input->canvas_width || !input->canvas_height) return;
    input->pointer_x = clamp(input->pointer_x - (float) y * input->mode.height / UINT16_MAX, 0,
                             input->canvas_width - 1);
    input->pointer_y = clamp(input->pointer_y + (float) x * input->mode.width / UINT16_MAX, 0,
                             input->canvas_height - 1);
}

void input_pan_configure(struct input_state *input, uint16_t width, uint16_t height, uint16_t x,
                         uint16_t y) {
    pthread_mutex_lock(&input->pan_mutex);
    input->canvas_width = width;
    input->canvas_height = height;
    input->pan_x = x;
    input->pan_y = y;
    input->pointer_x = x + input->mode.height / 2.0f;
    input->pointer_y = y + input->mode.width / 2.0f;
    follow(input);
    if (input->pan) atomic_store(&input->pan->rendered, (uint32_t) x << 16 | y);
    pthread_mutex_unlock(&input->pan_mutex);
}

void input_pan_ack(struct input_state *input, uint16_t x, uint16_t y, uint32_t sequence) {
    pthread_mutex_lock(&input->pan_mutex);
    uint32_t pending = input->move_sequence - sequence;
    if (pending < MOVE_HISTORY && input->canvas_width && input->canvas_height) {
        input->pointer_x = (float) x * (input->canvas_width - 1) / UINT16_MAX;
        input->pointer_y = (float) y * (input->canvas_height - 1) / UINT16_MAX;
        for (uint32_t i = 1; i <= pending; ++i) {
            uint32_t index = (sequence + i) % MOVE_HISTORY;
            if (input->moves[index].sequence == sequence + i)
                move(input, input->moves[index].x, input->moves[index].y);
        }
        follow(input);
    }
    pthread_mutex_unlock(&input->pan_mutex);
}

int input_send_move(struct input_state *input, int16_t x, int16_t y) {
    pthread_mutex_lock(&input->pan_mutex);
    uint32_t sequence = ++input->move_sequence;
    unsigned int index = sequence % MOVE_HISTORY;
    input->moves[index].sequence = sequence;
    input->moves[index].x = x;
    input->moves[index].y = y;
    move(input, x, y);
    follow(input);
    pthread_mutex_unlock(&input->pan_mutex);
    uint16_t dx = (uint16_t) x, dy = (uint16_t) y;
    uint8_t packet[] = {0x20, dx >> 8, dx, dy >> 8, dy, sequence >> 24, sequence >> 16,
                        sequence >> 8, sequence};
    return link_send(input->link, packet, sizeof(packet));
}

void input_pan_touch(struct input_state *input, uint8_t *packet) {
    if (!input->pan) return;
    uint32_t origin = atomic_load(&input->pan->rendered);
    for (unsigned int i = 0; i < packet[1]; ++i) {
        uint8_t *point = packet + 2 + i * PAN_POINT_SIZE;
        uint32_t x = ((uint32_t) point[2] << 8 | point[3]) + (origin >> 16);
        uint32_t y = ((uint32_t) point[4] << 8 | point[5]) + (origin & 65535);
        point[2] = x >> 8;
        point[3] = x;
        point[4] = y >> 8;
        point[5] = y;
    }
}