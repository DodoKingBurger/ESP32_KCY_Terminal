#pragma once

#include <stdint.h>


float parser_u16(
    const uint8_t *buf,
    uint16_t offset,
    float scale
);

float parser_float_abcd(
    const uint8_t *buf,
    uint16_t offset
);

float parser_float_cdab(
    const uint8_t *buf,
    uint16_t offset
);