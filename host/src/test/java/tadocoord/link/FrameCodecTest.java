package tadocoord.link;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;

class FrameCodecTest {

    @Test
    void crcIsCcittFalse() {
        byte[] check = "123456789".getBytes();
        assertEquals(0x29b1, FrameCodec.crc16(check, 0, check.length));
    }

    @Test
    void resyncsPastGarbageAndACorruptFrame() {
        byte[] corrupt = FrameCodec.encode(1, 0x81, new byte[] {0});
        corrupt[6] ^= 0x01; // flip a payload bit: CRC no longer matches
        ByteArrayOutputStream stream = new ByteArrayOutputStream();
        stream.writeBytes(new byte[] {0x00, 0x7e, 0x7e, (byte) 0xa5, (byte) 0xff, (byte) 0xff}); // noise, bad length
        stream.writeBytes(corrupt);
        stream.writeBytes(FrameCodec.encode(2, 0xc0, "hello".getBytes()));
        byte[] b = stream.toByteArray();

        List<FrameCodec.Frame> frames = new FrameCodec().feed(b, b.length);

        assertEquals(1, frames.size());
        assertEquals(2, frames.get(0).seq());
        assertArrayEquals("hello".getBytes(), frames.get(0).payload());
    }

    @Test
    void aFalseStartClaimingALongBodyDoesNotHoldBackTheNextFrame() {
        byte[] noise = {0x7e, (byte) 0xa5, 0x2c, 0x01, 0x11, 0x22}; // claims a 300-byte body
        byte[] real = FrameCodec.encode(4, 0x81, new byte[] {0});
        byte[] b = new byte[noise.length + real.length];
        System.arraycopy(noise, 0, b, 0, noise.length);
        System.arraycopy(real, 0, b, noise.length, real.length);

        List<FrameCodec.Frame> frames = new FrameCodec().feed(b, b.length);

        assertEquals(1, frames.size());
        assertEquals(4, frames.get(0).seq());
    }

    @Test
    void reassemblesAFrameSplitAcrossReads() {
        byte[] f = FrameCodec.encode(7, 0x84, new byte[] {0, 3, 9, 1, 0, 0, 0});
        FrameCodec codec = new FrameCodec();
        List<FrameCodec.Frame> got = new ArrayList<>();
        for (byte x : f) {
            got.addAll(codec.feed(new byte[] {x}, 1));
        }
        assertEquals(1, got.size());
        assertEquals(0x84, got.get(0).type());
        assertTrue(codec.feed(new byte[0], 0).isEmpty());
    }
}
