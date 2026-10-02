#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "video_control.h"
#include "audio.h"
#include "bytes.h"
#include "input.h"

int video_control(struct input_state *input, struct audio *audio, const uint8_t packet[10]) {
    struct view view;
    if (packet[0] == 0x11) {
        if (packet[1] == 1) audio_start(audio);
        else if (packet[1] == 0) audio_stop(audio);
        else return -1;
        return 1;
    }
    if (packet[0] == 0x12) {
        if (packet[1] == 1) {
            uint16_t width = read_u16(packet + 2), height = read_u16(packet + 4);
            if (width < input->mode.height || height < input->mode.width || width > 4096 || height > 4096) return -1;
            input_pan_configure(input, width, height, read_u16(packet + 6), read_u16(packet + 8));
        } else if (packet[1] == 2) {
            uint32_t sequence = read_u32(packet + 6);
            input_pan_ack(input, read_u16(packet + 2), read_u16(packet + 4), sequence);
        } else return -1;
        return 1;
    }
    if (packet[0] != 0x10 || packet[1] != 2) return -1;
    view.x = read_u16(packet + 2);
    view.y = read_u16(packet + 4);
    view.width = read_u16(packet + 6);
    view.height = read_u16(packet + 8);
    if (!view.width || !view.height || (uint32_t) view.x + view.width > input->mode.width ||
        (uint32_t) view.y + view.height > input->mode.height)
        return -1;
    input_set_view(input, view);
    return 0;
}
