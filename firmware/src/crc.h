#ifndef CRC_H
#define CRC_H

#include <stddef.h>
#include <stdint.h>

/* Host link CRC: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection).
 * Pass the previous result as init to continue over several buffers. */
#define CRC16_CCITT_FALSE_INIT 0xFFFFu
uint16_t crc16_ccitt_false(uint16_t crc, const uint8_t *data, size_t len);

/* tado MAC FCS: CRC-16/CCITT reflected (poly 0x8408), init 0, no final XOR.
 * Continue over several buffers the same way; start with 0. */
uint16_t crc16_mac_fcs(uint16_t crc, const uint8_t *data, size_t len);

#endif
