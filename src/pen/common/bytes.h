#ifndef PENDESK_BYTES_H
#define PENDESK_BYTES_H

#include <stdint.h>

static inline uint16_t read_u16(const uint8_t *source) {
    return (uint16_t)((uint16_t) source[0] << 8 | source[1]);
}

static inline uint32_t read_u32(const uint8_t *source) {
    return (uint32_t) source[0] << 24 | (uint32_t) source[1] << 16 | (uint32_t) source[2] << 8 |
           source[3];
}

static inline void write_u16(uint8_t *target, uint16_t value) {
    target[0] = (uint8_t)(value >> 8);
    target[1] = (uint8_t) value;
}

static inline void write_u32(uint8_t *target, uint32_t value) {
    target[0] = (uint8_t)(value >> 24);
    target[1] = (uint8_t)(value >> 16);
    target[2] = (uint8_t)(value >> 8);
    target[3] = (uint8_t) value;
}

#endif