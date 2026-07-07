#include "parser.h"

#include <string.h>

float parser_u16(
    const uint8_t *buf,
    uint16_t offset,
    float scale
)
{
    uint16_t raw =
        (buf[offset] << 8) |
        buf[offset + 1];

    return raw * scale;
}

float parser_float_abcd(
    const uint8_t *buf,
    uint16_t offset
)
{
    uint32_t raw =
        (buf[offset] << 24) |
        (buf[offset + 1] << 16) |
        (buf[offset + 2] << 8) |
        buf[offset + 3];

    float value;

    memcpy(
        &value,
        &raw,
        sizeof(value)
    );

    return value;
}

/*
 * CDAB
 * very common in Modbus devices
 */

float parser_float_cdab(
    const uint8_t *buf,
    uint16_t offset
)
{
    uint8_t tmp[4];

    tmp[0] = buf[offset + 2];
    tmp[1] = buf[offset + 3];

    tmp[2] = buf[offset + 0];
    tmp[3] = buf[offset + 1];

    float value;

    memcpy(
        &value,
        tmp,
        sizeof(value)
    );

    return value;
}