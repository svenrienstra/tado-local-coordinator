package tadocoord;

import java.security.SecureRandom;
import java.util.Arrays;

/**
 * An EUI-64 in wire (little-endian) order, as it appears in MAC headers.
 *
 * <p>Every tado node address ends in {@code c5 1b 00}: a unicast header carries only the first 5
 * source bytes and the receiver appends the suffix, so the coordinator must use it too.
 */
public record Eui(byte[] wire) {
    public static final byte[] SUFFIX = {(byte) 0xc5, 0x1b, 0x00};

    public Eui {
        if (wire.length != 8) {
            throw new IllegalArgumentException("EUI must be 8 bytes");
        }
        wire = wire.clone();
    }

    public static Eui ofHex(String hex) {
        return new Eui(Hex.parse(hex));
    }

    /** A fresh coordinator address: 5 random bytes followed by the tado suffix. */
    public static Eui randomCoordinator(SecureRandom rng) {
        byte[] w = new byte[8];
        rng.nextBytes(w);
        System.arraycopy(SUFFIX, 0, w, 5, 3);
        return new Eui(w);
    }

    public boolean hasTadoSuffix() {
        return wire[5] == SUFFIX[0] && wire[6] == SUFFIX[1] && wire[7] == SUFFIX[2];
    }

    /** The EUI in printed (big-endian) order, as IPv6 and ICMPv6 options carry it. */
    public byte[] bigEndian() {
        byte[] be = new byte[8];
        for (int i = 0; i < 8; i++) {
            be[i] = wire[7 - i];
        }
        return be;
    }

    /** fe80::/64 with the interface id from the big-endian EUI, U/L bit flipped. */
    public byte[] linkLocal() {
        byte[] a = new byte[16];
        a[0] = (byte) 0xfe;
        a[1] = (byte) 0x80;
        System.arraycopy(bigEndian(), 0, a, 8, 8);
        a[8] ^= 0x02;
        return a;
    }

    @Override
    public byte[] wire() {
        return wire.clone();
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof Eui e && Arrays.equals(wire, e.wire);
    }

    @Override
    public int hashCode() {
        return Arrays.hashCode(wire);
    }

    @Override
    public String toString() {
        return Hex.of(wire);
    }
}
