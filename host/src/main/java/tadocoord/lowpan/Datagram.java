package tadocoord.lowpan;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Optional;
import java.util.OptionalInt;
import tadocoord.Eui;

/**
 * A 6LoWPAN (RFC 6282 IPHC) datagram as tado nodes send it.
 *
 * <p>The coordinator's own requests and answers use one form: link-local addresses elided from
 * the L2 addresses, hop limit 64, UDP ports inline, UDP checksum inline ({@code 7e33 f0}).
 * Inbound datagrams are parsed for any stateless form; addresses compressed against a context
 * (zone plane) are not reconstructed, so their {@code src}/{@code dst} stay empty.
 *
 * @param nextHeader {@link Ipv6#NH_UDP} or {@link Ipv6#NH_ICMPV6} (or another value, unparsed)
 * @param srcPort UDP only, else 0
 * @param dstPort UDP only, else 0
 * @param payload the UDP payload, or the full ICMPv6 message
 */
public record Datagram(int nextHeader, Optional<byte[]> src, Optional<byte[]> dst,
        int srcPort, int dstPort, byte[] payload) {

    public static final int COAP_PORT = 5683;

    private static final int IPHC_LINK_LOCAL_0 = 0x7e; // TF elided, NH compressed, hop limit 64
    private static final int IPHC_LINK_LOCAL_1 = 0x33; // SAM=3, DAM=3: both from L2
    private static final int NHC_UDP_PORTS_INLINE = 0xf0;
    private static final int IPHC_ICMP_0 = 0x7b; // TF elided, NH inline, hop limit 255
    private static final int IPHC_MCAST_8BIT_1 = 0x3b; // SAM=3 from L2; M=1, DAM=3: ff02::00XX

    /** Builds a link-local UDP datagram from {@code src} to {@code dst}, as the bridge does. */
    public static byte[] udp(Eui src, Eui dst, int srcPort, int dstPort, byte[] payload) {
        byte[] udp = ByteBuffer.allocate(8 + payload.length)
                .putShort((short) srcPort).putShort((short) dstPort).putShort((short) (8 + payload.length))
                .putShort((short) 0).put(payload).array();
        int ck = Ipv6.checksum(src.linkLocal(), dst.linkLocal(), Ipv6.NH_UDP, udp);
        return ByteBuffer.allocate(9 + payload.length)
                .put((byte) IPHC_LINK_LOCAL_0).put((byte) IPHC_LINK_LOCAL_1).put((byte) NHC_UDP_PORTS_INLINE)
                .put(udp, 0, 4).putShort((short) ck).put(payload).array();
    }

    /**
     * Builds an ICMPv6 datagram from {@code src} to the link-scope group ff02::{@code group}, hop
     * limit 255 (RFC 4861). {@code message} has a zero checksum field; the checksum is filled in.
     */
    public static byte[] icmpv6ToLinkMulticast(Eui src, int group, byte[] message) {
        byte[] msg = message.clone();
        int ck = Ipv6.checksum(src.linkLocal(), Ipv6.linkScopeMulticast(group), Ipv6.NH_ICMPV6, msg);
        msg[2] = (byte) (ck >>> 8);
        msg[3] = (byte) ck;
        return ByteBuffer.allocate(4 + msg.length)
                .put((byte) IPHC_ICMP_0).put((byte) IPHC_MCAST_8BIT_1).put((byte) Ipv6.NH_ICMPV6).put((byte) group)
                .put(msg).array();
    }

    /** The ICMPv6 type, for an ICMPv6 datagram. */
    public OptionalInt icmpType() {
        return nextHeader == Ipv6.NH_ICMPV6 && payload.length > 0 ? OptionalInt.of(payload[0] & 0xff)
                : OptionalInt.empty();
    }

    /**
     * Parses an inbound datagram. {@code l2Src}/{@code l2Dst} are the MAC addresses the
     * elided IPv6 addresses derive from ({@code l2Dst} is empty for a broadcast).
     */
    public static Datagram parse(byte[] p, Eui l2Src, Optional<Eui> l2Dst) {
        Reader r = new Reader(p);
        int b0 = r.u8();
        int b1 = r.u8();
        if ((b0 & 0xe0) != 0x60) {
            throw new IllegalArgumentException("not an IPHC datagram");
        }
        if ((b1 & 0x80) != 0) {
            r.skip(1); // context identifier extension
        }
        int tf = (b0 >>> 3) & 3;
        r.skip(switch (tf) { case 0 -> 4; case 1 -> 3; case 2 -> 1; default -> 0; });
        boolean nhCompressed = (b0 & 0x04) != 0;
        int nextHeader = nhCompressed ? -1 : r.u8();
        if ((b0 & 3) == 0) {
            r.skip(1); // inline hop limit
        }

        boolean sac = (b1 & 0x40) != 0;
        int sam = (b1 >>> 4) & 3;
        Optional<byte[]> src = address(r, sac, false, sam, Optional.of(l2Src));
        boolean m = (b1 & 0x08) != 0;
        boolean dac = (b1 & 0x04) != 0;
        int dam = b1 & 3;
        Optional<byte[]> dst = address(r, dac, m, dam, l2Dst);

        if (nhCompressed) {
            int nhc = r.u8();
            if ((nhc & 0xf8) != 0xf0) {
                throw new IllegalArgumentException("unsupported NHC 0x" + Integer.toHexString(nhc));
            }
            int sp;
            int dp;
            switch (nhc & 3) {
                case 0 -> { sp = r.u16(); dp = r.u16(); }
                case 1 -> { sp = r.u16(); dp = 0xf000 | r.u8(); }
                case 2 -> { sp = 0xf000 | r.u8(); dp = r.u16(); }
                default -> { int b = r.u8(); sp = 0xf0b0 | (b >>> 4); dp = 0xf0b0 | (b & 0xf); }
            }
            if ((nhc & 0x04) == 0) {
                r.skip(2); // inline UDP checksum; the MIC already covers the frame
            }
            return new Datagram(Ipv6.NH_UDP, src, dst, sp, dp, r.rest());
        }
        if (nextHeader == Ipv6.NH_UDP) {
            int sp = r.u16();
            int dp = r.u16();
            r.skip(4); // length and checksum: the frame and its MIC bound the datagram
            return new Datagram(Ipv6.NH_UDP, src, dst, sp, dp, r.rest());
        }
        return new Datagram(nextHeader, src, dst, 0, 0, r.rest());
    }

    private static Optional<byte[]> address(Reader r, boolean stateful, boolean multicast, int mode,
            Optional<Eui> l2) {
        if (multicast && stateful) {
            // RFC 6282: only DAM=00 is defined (48 bits inline, prefix from a context).
            if (mode != 0) {
                throw new IllegalArgumentException("reserved stateful multicast mode " + mode);
            }
            r.skip(6);
            return Optional.empty();
        }
        if (multicast) {
            byte[] a = new byte[16];
            a[0] = (byte) 0xff;
            switch (mode) {
                case 0 -> { return Optional.of(r.bytes(16)); }
                case 1 -> { a[1] = (byte) r.u8(); System.arraycopy(r.bytes(5), 0, a, 11, 5); }
                case 2 -> { a[1] = (byte) r.u8(); System.arraycopy(r.bytes(3), 0, a, 13, 3); }
                default -> { return Optional.of(Ipv6.linkScopeMulticast(r.u8())); }
            }
            return Optional.of(a);
        }
        if (stateful) {
            // Context-based: the prefix lives in a context we do not track yet.
            r.skip(switch (mode) { case 1 -> 8; case 2 -> 2; default -> 0; });
            return Optional.empty();
        }
        byte[] a = new byte[16];
        a[0] = (byte) 0xfe;
        a[1] = (byte) 0x80;
        switch (mode) {
            case 0 -> { return Optional.of(r.bytes(16)); }
            case 1 -> System.arraycopy(r.bytes(8), 0, a, 8, 8);
            case 2 -> { a[11] = (byte) 0xff; a[12] = (byte) 0xfe; System.arraycopy(r.bytes(2), 0, a, 14, 2); }
            default -> { return l2.map(Eui::linkLocal); }
        }
        return Optional.of(a);
    }

    private static final class Reader {
        private final byte[] b;
        private int i;

        Reader(byte[] b) {
            this.b = b;
        }

        int u8() {
            if (i >= b.length) {
                throw new IllegalArgumentException("truncated datagram");
            }
            return b[i++] & 0xff;
        }

        int u16() {
            return (u8() << 8) | u8();
        }

        byte[] bytes(int n) {
            if (i + n > b.length) {
                throw new IllegalArgumentException("truncated datagram");
            }
            byte[] out = Arrays.copyOfRange(b, i, i + n);
            i += n;
            return out;
        }

        void skip(int n) {
            bytes(n);
        }

        byte[] rest() {
            return bytes(b.length - i);
        }
    }
}
