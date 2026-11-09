package tadocoord.lowpan;

/** IPv6 constants and the pseudo-header checksum shared by UDP and ICMPv6. */
public final class Ipv6 {
    public static final int NH_UDP = 17;
    public static final int NH_ICMPV6 = 58;

    private Ipv6() {}

    /** ff02::xx */
    public static byte[] linkScopeMulticast(int group) {
        byte[] a = new byte[16];
        a[0] = (byte) 0xff;
        a[1] = 0x02;
        a[15] = (byte) group;
        return a;
    }

    /**
     * RFC 8200 upper-layer checksum over the pseudo-header and {@code upper}, whose checksum field
     * must be zero. A UDP result of 0 is sent as 0xffff (RFC 768); ICMPv6 keeps 0.
     */
    public static int checksum(byte[] src, byte[] dst, int nextHeader, byte[] upper) {
        long sum = 0;
        sum += sum16(src) + sum16(dst);
        sum += (upper.length >>> 16) + (upper.length & 0xffff);
        sum += nextHeader;
        sum += sum16(upper);
        while ((sum >>> 16) != 0) {
            sum = (sum & 0xffff) + (sum >>> 16);
        }
        int c = (int) (~sum & 0xffff);
        return (nextHeader == NH_UDP && c == 0) ? 0xffff : c;
    }

    private static long sum16(byte[] b) {
        long s = 0;
        for (int i = 0; i + 1 < b.length; i += 2) {
            s += ((b[i] & 0xff) << 8) | (b[i + 1] & 0xff);
        }
        if ((b.length & 1) == 1) {
            s += (b[b.length - 1] & 0xff) << 8;
        }
        return s;
    }
}
