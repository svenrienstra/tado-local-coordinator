/* aes128_encrypt_block() on the CC1312 AES engine (ECB, one block). */
#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/aes.h)
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>

#include "ccm.h"
#include "platform.h"

/* The engine DMAs the key from RAM; keeping a copy also lets consecutive
 * blocks under one key skip the key-store write. */
static uint8_t loaded_key[16];
static bool key_valid;

void aes_hw_init(void)
{
    Power_setDependency(PowerCC26XX_PERIPH_CRYPTO);
}

void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t buf_in[16];
    if (!key_valid || memcmp(key, loaded_key, 16) != 0) {
        memcpy(loaded_key, key, 16);
        AESWriteToKeyStore(loaded_key, AES_128_KEY_LENGTH_BYTES, AES_KEY_AREA_0);
        key_valid = true;
    }
    memcpy(buf_in, in, 16);
    AESSelectAlgorithm(AES_ALGSEL_AES);
    AESReadFromKeyStore(AES_KEY_AREA_0);
    AESSetCtrl(CRYPTO_AESCTL_DIR); /* encrypt, no chaining mode = ECB */
    AESSetDataLength(16);
    AESSetAuthLength(0);
    AESStartDMAOperation(buf_in, 16, out, 16);
    AESWaitForIRQFlags(AES_RESULT_RDY | AES_DMA_BUS_ERR);
    AESSelectAlgorithm(0);
}
