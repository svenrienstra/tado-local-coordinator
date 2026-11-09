package tadocoord.link;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Host-link framing (docs/HOSTLINK.md): {@code 7E A5 | len u16 | body | crc u16}, little-endian,
 * CRC-16/CCITT-FALSE over len and body. The decoder resyncs by dropping one byte at a time.
 */
public final class FrameCodec {
    static final int MAX_BODY = 300;
    private static final int SOF0 = 0x7e;
    private static final int SOF1 = 0xa5;

    /** A decoded body: sequence number, type and payload. */
    public record Frame(int seq, int type, byte[] payload) {}

    /** Unconsumed bytes are {@code buf[0..len)}; at most one partial frame plus one read. */
    private byte[] buf = new byte[2 * (4 + MAX_BODY + 2)];
    private int len;

    public static byte[] encode(int seq, int type, byte[] payload) {
        int bodyLen = 2 + payload.length;
        if (bodyLen > MAX_BODY) {
            throw new IllegalArgumentException("body exceeds " + MAX_BODY + " bytes");
        }
        ByteBuffer f = ByteBuffer.allocate(4 + bodyLen + 2).order(ByteOrder.LITTLE_ENDIAN);
        f.put((byte) SOF0).put((byte) SOF1).putShort((short) bodyLen).put((byte) seq).put((byte) type).put(payload);
        f.putShort((short) crc16(f.array(), 2, 2 + bodyLen));
        return f.array();
    }

    /** Feeds received bytes and returns every complete, valid frame. */
    public List<Frame> feed(byte[] data, int n) {
        if (len + n > buf.length) {
            buf = Arrays.copyOf(buf, len + n);
        }
        System.arraycopy(data, 0, buf, len, n);
        len += n;
        List<Frame> out = List.of();
        int i = 0;
        while (len - i >= 4) {
            if ((buf[i] & 0xff) != SOF0 || (buf[i + 1] & 0xff) != SOF1) {
                i++;
                continue;
            }
            int bodyLen = (buf[i + 2] & 0xff) | ((buf[i + 3] & 0xff) << 8);
            if (bodyLen < 2 || bodyLen > MAX_BODY) {
                i++;
                continue;
            }
            int total = 4 + bodyLen + 2;
            if (len - i < total) {
                // Maybe a false start inside noise, claiming a long body: a complete, valid frame
                // further on proves it, so skip to that instead of holding it back.
                int next = nextValidFrame(i + 1);
                if (next < 0) {
                    break; // wait for the rest
                }
                i = next;
                continue;
            }
            int want = (buf[i + total - 2] & 0xff) | ((buf[i + total - 1] & 0xff) << 8);
            if (crc16(buf, i + 2, 2 + bodyLen) != want) {
                i++;
                continue;
            }
            if (out.isEmpty()) {
                out = new ArrayList<>();
            }
            out.add(new Frame(buf[i + 4] & 0xff, buf[i + 5] & 0xff, Arrays.copyOfRange(buf, i + 6, i + 4 + bodyLen)));
            i += total;
        }
        System.arraycopy(buf, i, buf, 0, len - i);
        len -= i;
        return out;
    }

    /** Start of the first complete frame with a good CRC at or after {@code from}, or -1. */
    private int nextValidFrame(int from) {
        for (int j = from; len - j >= 4; j++) {
            if ((buf[j] & 0xff) != SOF0 || (buf[j + 1] & 0xff) != SOF1) {
                continue;
            }
            int bodyLen = (buf[j + 2] & 0xff) | ((buf[j + 3] & 0xff) << 8);
            int total = 4 + bodyLen + 2;
            if (bodyLen >= 2 && bodyLen <= MAX_BODY && len - j >= total) {
                int want = (buf[j + total - 2] & 0xff) | ((buf[j + total - 1] & 0xff) << 8);
                if (crc16(buf, j + 2, 2 + bodyLen) == want) {
                    return j;
                }
            }
        }
        return -1;
    }

    static int crc16(byte[] b, int off, int len) {
        int crc = 0xffff;
        for (int i = off; i < off + len; i++) {
            crc ^= (b[i] & 0xff) << 8;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1;
            }
        }
        return crc & 0xffff;
    }
}
