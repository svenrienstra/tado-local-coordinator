package tadocoord.lowpan;

import java.io.ByteArrayOutputStream;
import java.time.Duration;
import java.time.Instant;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;
import tadocoord.Eui;

/**
 * Reassembles RFC 4944 fragmented datagrams into one compressed (IPHC) datagram.
 *
 * <p>Size and offsets count bytes of the uncompressed IPv6 datagram. FRAG1 carries the
 * compressed headers, which stand for the 40-byte IPv6 header (plus 8 for UDP), and the first
 * payload bytes; each FRAGN carries later payload bytes at its offset. A MAC retransmission
 * delivers a fragment twice, which is ignored. Not thread-safe.
 */
public final class Reassembler {
    /** RFC 4944 reassembly timeout. */
    static final Duration TIMEOUT = Duration.ofSeconds(60);
    static final int MAX_SIZE = 1280;

    private static final int FRAG1 = 0xc0;
    private static final int FRAGN = 0xe0;
    private static final int FRAG1_HEADER = 4;
    private static final int FRAGN_HEADER = 5;
    private static final int IPV6_HEADER = 40;
    private static final int UDP_HEADER = 8;

    private record Key(Eui src, int size, int tag) {}

    private static final class Partial {
        final Instant started;
        byte[] frag1; // compressed headers and the first payload bytes
        int frag1Covers; // uncompressed bytes frag1 stands for
        final TreeMap<Integer, byte[]> later = new TreeMap<>(); // FRAGN payload by offset

        Partial(Instant started) {
            this.started = started;
        }
    }

    private final Map<Key, Partial> partials = new HashMap<>();

    public static boolean isFragment(byte[] p) {
        int d = p.length > 0 ? p[0] & 0xf8 : 0;
        return d == FRAG1 || d == FRAGN;
    }

    /**
     * Files one packet from {@code src}. A packet that is no fragment comes back as is; a
     * fragment returns the whole datagram once it completes one, else empty.
     *
     * @throws IllegalArgumentException for a malformed fragment, or one that overlaps a
     *     different fragment of its datagram (the datagram is then dropped)
     */
    public Optional<byte[]> add(Eui src, byte[] p, Instant now) {
        if (!isFragment(p)) {
            return Optional.of(p);
        }
        partials.values().removeIf(x -> !now.isBefore(x.started.plus(TIMEOUT)));
        boolean first = (p[0] & 0xf8) == FRAG1;
        if (p.length < (first ? FRAG1_HEADER : FRAGN_HEADER)) {
            throw new IllegalArgumentException("truncated fragment header");
        }
        int size = ((p[0] & 0x07) << 8) | (p[1] & 0xff);
        int tag = ((p[2] & 0xff) << 8) | (p[3] & 0xff);
        if (size > MAX_SIZE) {
            throw new IllegalArgumentException("fragmented datagram of " + size + " bytes");
        }
        Key key = new Key(src, size, tag);
        Partial x = partials.computeIfAbsent(key, k -> new Partial(now));
        try {
            if (first) {
                addFirst(x, Arrays.copyOfRange(p, FRAG1_HEADER, p.length), src, size);
            } else {
                addLater(x, (p[4] & 0xff) * 8, Arrays.copyOfRange(p, FRAGN_HEADER, p.length), size);
            }
        } catch (IllegalArgumentException e) {
            partials.remove(key);
            throw e;
        }
        Optional<byte[]> whole = assemble(x, size);
        whole.ifPresent(w -> partials.remove(key));
        return whole;
    }

    private static void addFirst(Partial x, byte[] body, Eui src, int size) {
        if (x.frag1 != null) {
            if (!Arrays.equals(x.frag1, body)) {
                throw new IllegalArgumentException("two different first fragments");
            }
            return;
        }
        Datagram head = Datagram.parse(body, src, Optional.empty());
        int covers = IPV6_HEADER + (head.nextHeader() == Ipv6.NH_UDP ? UDP_HEADER : 0) + head.payload().length;
        if (covers > size) {
            throw new IllegalArgumentException("first fragment longer than its datagram");
        }
        x.frag1 = body;
        x.frag1Covers = covers;
        checkOverlap(x);
    }

    private static void addLater(Partial x, int offset, byte[] data, int size) {
        if (offset + data.length > size) {
            throw new IllegalArgumentException("fragment past the end of its datagram");
        }
        byte[] seen = x.later.putIfAbsent(offset, data);
        if (seen != null && !Arrays.equals(seen, data)) {
            throw new IllegalArgumentException("two different fragments at offset " + offset);
        }
        checkOverlap(x);
    }

    /** Pieces in offset order must not overlap: each starts at or after the previous end. */
    private static void checkOverlap(Partial x) {
        int end = x.frag1 != null ? x.frag1Covers : 0;
        for (Map.Entry<Integer, byte[]> e : x.later.entrySet()) {
            if (e.getKey() < end) {
                throw new IllegalArgumentException("overlapping fragment at offset " + e.getKey());
            }
            end = e.getKey() + e.getValue().length;
        }
    }

    private static Optional<byte[]> assemble(Partial x, int size) {
        if (x.frag1 == null) {
            return Optional.empty();
        }
        int end = x.frag1Covers;
        for (Map.Entry<Integer, byte[]> e : x.later.entrySet()) {
            if (e.getKey() != end) {
                return Optional.empty(); // a gap: a fragment is still missing
            }
            end += e.getValue().length;
        }
        if (end != size) {
            return Optional.empty();
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.writeBytes(x.frag1);
        x.later.values().forEach(out::writeBytes);
        return Optional.of(out.toByteArray());
    }
}
