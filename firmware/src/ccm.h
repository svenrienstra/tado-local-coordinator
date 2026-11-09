#ifndef CCM_H
#define CCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CCM* as the tado link uses it: AES-128, 4-byte MIC, L=2, B0 flags 0x49.
 * The nonce is the first 13 header bytes and the aad the first 16. */
#define CCM_NONCE_LEN 13
#define CCM_MIC_LEN   4

/* One AES-128 block encryption. Provided by the platform: the hardware AES
 * engine on the CC1312, a software AES in the host tests. */
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* Encrypts buf in place and writes the MIC. */
void ccm_seal(const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
              const uint8_t *aad, size_t aad_len, uint8_t *buf, size_t len,
              uint8_t mic[CCM_MIC_LEN]);

/* Decrypts buf in place and checks the MIC. On false buf holds garbage, so a
 * caller that tries several keys works on a copy. */
bool ccm_open(const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
              const uint8_t *aad, size_t aad_len, uint8_t *buf, size_t len,
              const uint8_t mic[CCM_MIC_LEN]);

#endif
