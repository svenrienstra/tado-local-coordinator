#include "crc.h"

/* Nibble tables: the CRC of each 4-bit value, two lookups per byte. 32 bytes
 * each instead of 512 for byte tables, and still well ahead of bit-by-bit on
 * the path to the Enhanced ACK. */

static const uint16_t ccitt_false_nibble[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
};

static const uint16_t mac_fcs_nibble[16] = {
    0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
    0x8408, 0x9489, 0xa50a, 0xb58b, 0xc60c, 0xd68d, 0xe70e, 0xf78f,
};

uint16_t crc16_ccitt_false(uint16_t crc, const uint8_t *data, size_t len)
{
    /* MSB first: high nibble, then low. */
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc << 4) ^ ccitt_false_nibble[(crc >> 12) ^ (data[i] >> 4)];
        crc = (uint16_t)(crc << 4) ^ ccitt_false_nibble[(crc >> 12) ^ (data[i] & 0x0fu)];
    }
    return crc;
}

uint16_t crc16_mac_fcs(uint16_t crc, const uint8_t *data, size_t len)
{
    /* Reflected, LSB first: low nibble, then high. */
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 4) ^ mac_fcs_nibble[(crc ^ data[i]) & 0x0fu];
        crc = (crc >> 4) ^ mac_fcs_nibble[(crc ^ (data[i] >> 4)) & 0x0fu];
    }
    return crc;
}
