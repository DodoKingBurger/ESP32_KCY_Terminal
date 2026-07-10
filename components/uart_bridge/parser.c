#include "parser.h"

#include <string.h>

/**
 * @brief Преобразование двух байт (big-endian) в float с масштабированием
 * @param buf    буфер
 * @param offset смещение в байтах
 * @param scale  коэффициент масштабирования
 * @return float = (uint16_t) * scale
 */
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

/**
 * @brief Преобразование 4 байт (ABCD) в float (IEEE 754)
 * @param buf    буфер
 * @param offset смещение
 * @return float
 */
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


/**
 * @brief Преобразование 4 байт (CDAB) в float (порядок байт, часто встречается в Modbus)
 * @param buf    буфер
 * @param offset смещение
 * @return float
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