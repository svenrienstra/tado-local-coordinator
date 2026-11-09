package tadocoord.coap;

import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

class CoapMessageTest {

    @Test
    void aTruncatedExtendedOptionHeaderIsRejectedNotThrownAsIndexError() {
        // CON GET, MID 0, then an option header with a 1-byte extended delta (nibble 13) and nothing after it.
        byte[] truncated = {0x40, 0x01, 0x00, 0x00, (byte) 0xd0};
        assertThrows(IllegalArgumentException.class, () -> CoapMessage.decode(truncated));
    }
}
