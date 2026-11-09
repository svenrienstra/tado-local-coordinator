#include "ccm.h"

#include <string.h>

#define B0_FLAGS  0x49 /* Adata=1, M=4, L=2 */
#define CTR_FLAGS 0x01 /* L-1 */

static void ctr_block(uint8_t a[16], const uint8_t nonce[CCM_NONCE_LEN], uint16_t i)
{
    a[0] = CTR_FLAGS;
    memcpy(&a[1], nonce, CCM_NONCE_LEN);
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
}

/* XORs up to 16 bytes into the CBC-MAC state and encrypts it. */
static void cbc_step(const uint8_t key[16], uint8_t x[16], const uint8_t *data, size_t n)
{
    for (size_t k = 0; k < n; k++)
        x[k] ^= data[k];
    aes128_encrypt_block(key, x, x);
}

static void mic_compute(const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                        const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t len,
                        uint8_t mic[CCM_MIC_LEN])
{
    uint8_t x[16];
    x[0] = B0_FLAGS;
    memcpy(&x[1], nonce, CCM_NONCE_LEN);
    x[14] = (uint8_t)(len >> 8);
    x[15] = (uint8_t)len;
    aes128_encrypt_block(key, x, x);

    /* The aad is prefixed with its 2-byte length; the blocks are zero-padded,
     * which XORing fewer bytes into the state gives for free. */
    uint8_t blk[16] = {(uint8_t)(aad_len >> 8), (uint8_t)aad_len};
    size_t fill = 2, i = 0;
    while (i < aad_len) {
        size_t n = aad_len - i < 16 - fill ? aad_len - i : 16 - fill;
        memcpy(&blk[fill], &aad[i], n);
        cbc_step(key, x, blk, fill + n);
        i += n;
        fill = 0;
    }
    for (i = 0; i < len; i += 16)
        cbc_step(key, x, &pt[i], len - i < 16 ? len - i : 16);

    uint8_t a[16], s0[16];
    ctr_block(a, nonce, 0);
    aes128_encrypt_block(key, a, s0);
    for (int k = 0; k < CCM_MIC_LEN; k++)
        mic[k] = x[k] ^ s0[k];
}

static void ctr_xor(const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN], uint8_t *buf, size_t len)
{
    uint8_t a[16], ks[16];
    for (size_t i = 0; i < len; i += 16) {
        ctr_block(a, nonce, (uint16_t)(i / 16 + 1));
        aes128_encrypt_block(key, a, ks);
        for (size_t k = 0; k < 16 && i + k < len; k++)
            buf[i + k] ^= ks[k];
    }
}

void ccm_seal(const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
              const uint8_t *aad, size_t aad_len, uint8_t *buf, size_t len,
              uint8_t mic[CCM_MIC_LEN])
{
    mic_compute(key, nonce, aad, aad_len, buf, len, mic);
    ctr_xor(key, nonce, buf, len);
}

bool ccm_open(const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
              const uint8_t *aad, size_t aad_len, uint8_t *buf, size_t len,
              const uint8_t mic[CCM_MIC_LEN])
{
    uint8_t want[CCM_MIC_LEN];
    ctr_xor(key, nonce, buf, len);
    mic_compute(key, nonce, aad, aad_len, buf, len, want);
    return memcmp(want, mic, CCM_MIC_LEN) == 0;
}
