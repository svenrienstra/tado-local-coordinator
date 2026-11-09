package tadocoord.link;

import java.io.BufferedWriter;
import java.io.Closeable;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.InstantSource;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import tadocoord.Eui;
import tadocoord.Hex;

/**
 * Writes every host-link frame, both directions, as one timestamped line: the record to read
 * when something goes wrong on the radio side. Every occurrence of a secret (the network key,
 * which travels in {@code CONFIG} and in plaintext inside {@code d/pair}) is masked.
 *
 * <p>A failing write stops the capture with one warning; it never takes the radio link down.
 */
public final class LinkCapture implements HostLink.FrameTap, Closeable {
    private static final Map<Integer, String> NAMES = Map.ofEntries(
            Map.entry(HostLink.PING, "PING"), Map.entry(HostLink.CONFIG, "CONFIG"),
            Map.entry(HostLink.PAIR_WINDOW, "PAIR_WINDOW"), Map.entry(HostLink.TX, "TX"),
            Map.entry(HostLink.STATS, "STATS"),
            Map.entry(Event.LOG, "LOG"), Map.entry(Event.RX, "RX"),
            Map.entry(Event.BEACON_ANSWERED, "BEACON_ANSWERED"),
            Map.entry(Event.RX_UNVERIFIED, "RX_UNVERIFIED"), Map.entry(Event.TX_DONE, "TX_DONE"));
    private static final int UNVERIFIED_FRAME_OFFSET = 5; // rssi(1) rat(4)

    private final BufferedWriter out;
    private final InstantSource clock;
    private final List<byte[]> secrets;
    private boolean failed;

    public LinkCapture(Path file, InstantSource clock, List<byte[]> secrets) throws IOException {
        Files.createDirectories(file.getParent());
        this.out = Files.newBufferedWriter(file, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        this.clock = clock;
        this.secrets = secrets.stream().map(byte[]::clone).toList();
    }

    @Override
    public synchronized void frame(boolean outbound, int seq, int type, byte[] payload) {
        if (failed) {
            return;
        }
        try {
            out.write(line(clock.instant().toString(), outbound, seq, type, payload, secrets));
            out.newLine();
            out.flush(); // a crash must not lose the lines that explain it
        } catch (IOException e) {
            failed = true;
            System.err.println("host-link capture stopped: " + e);
        }
    }

    static String line(String time, boolean outbound, int seq, int type, byte[] payload, List<byte[]> secrets) {
        boolean reply = !outbound && (type & HostLink.EVENT_MASK) == HostLink.REPLY_FLAG;
        int base = reply ? type & ~HostLink.REPLY_FLAG : type;
        String name = NAMES.getOrDefault(base, "0x%02x".formatted(base)) + (reply ? "_REPLY" : "");
        byte[] shown = payload.clone();
        if (mask(shown, secrets)) {
            name += " (key redacted)";
        }
        String detail = switch (outbound ? -1 : type) {
            case Event.LOG -> " \"" + new String(payload, StandardCharsets.US_ASCII) + "\"";
            case Event.RX_UNVERIFIED -> payload.length > UNVERIFIED_FRAME_OFFSET
                    ? " rssi=" + payload[0] + " " + describeMac(Arrays.copyOfRange(payload, UNVERIFIED_FRAME_OFFSET, payload.length))
                    : "";
            default -> "";
        };
        return "%s %s seq=%02x %s%s %s".formatted(time, outbound ? ">" : "<", seq, name, detail, Hex.of(shown));
    }

    /** Zeroes every occurrence of each secret in {@code b}; true if any was found. */
    static boolean mask(byte[] b, List<byte[]> secrets) {
        boolean found = false;
        for (byte[] s : secrets) {
            for (int i = 0; s.length > 0 && i + s.length <= b.length; i++) {
                if (Arrays.equals(b, i, i + s.length, s, 0, s.length)) {
                    Arrays.fill(b, i, i + s.length, (byte) 0);
                    found = true;
                }
            }
        }
        return found;
    }

    /** What a raw MAC frame is, from its clear header only. */
    static String describeMac(byte[] f) {
        if (f.length < 2) {
            return "short";
        }
        int fcf = ((f[0] & 0xff) << 8) | (f[1] & 0xff);
        return switch (fcf) {
            case 0x69ec, 0x79ec -> f.length >= 16
                    ? "data dst=" + eui(f, 3) + " src=" + Hex.of(Arrays.copyOfRange(f, 11, 16)) + Hex.of(Eui.SUFFIX)
                    : "data (short)";
            case 0x49e8, 0x59e8 -> f.length >= 13 ? "broadcast src=" + eui(f, 5) : "broadcast (short)";
            case 0x0be8 -> f.length >= 15 ? "beacon-request src=" + eui(f, 7) : "beacon-request (short)";
            case 0x18ec -> "enhanced-beacon";
            case 0x42ee -> f.length >= 11 ? "enh-ack dst=" + eui(f, 3) : "enh-ack (short)";
            default -> (f[0] & 0xff) == 0x25 ? "wake-up" : "fcf=%04x".formatted(fcf);
        };
    }

    private static String eui(byte[] f, int off) {
        return Hex.of(Arrays.copyOfRange(f, off, off + 8));
    }

    @Override
    public synchronized void close() throws IOException {
        out.close();
    }
}
