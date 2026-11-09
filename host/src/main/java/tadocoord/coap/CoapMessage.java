package tadocoord.coap;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.stream.Collectors;

/**
 * A CoAP message (RFC 7252) with the subset tado uses: no observe or block handling here, just
 * the header, options in number order, and the payload.
 */
public record CoapMessage(Type type, int code, int mid, byte[] token, List<Option> options, byte[] payload) {

    public enum Type { CON, NON, ACK, RST }

    public static final int URI_PATH = 11;
    public static final int CONTENT_FORMAT = 12;
    /** tado session id; above every standard option, so it always encodes last. */
    public static final int SESSION = 2048;

    public record Option(int number, byte[] value) {}

    /** Code from its dotted form, e.g. {@code code(2, 5)} for 2.05. */
    public static int code(int cls, int detail) {
        return (cls << 5) | detail;
    }

    public static String codeString(int code) {
        return (code >>> 5) + "." + String.format("%02d", code & 0x1f);
    }

    public static Option path(String segment) {
        return new Option(URI_PATH, segment.getBytes(StandardCharsets.UTF_8));
    }

    public String path() {
        return options.stream().filter(o -> o.number() == URI_PATH)
                .map(o -> new String(o.value(), StandardCharsets.UTF_8))
                .collect(Collectors.joining("/"));
    }

    public byte[] encode() {
        if (token.length > 8) {
            throw new IllegalArgumentException("token longer than 8 bytes");
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(0x40 | (type.ordinal() << 4) | token.length);
        out.write(code);
        out.write(mid >>> 8);
        out.write(mid);
        out.writeBytes(token);
        List<Option> sorted = new ArrayList<>(options);
        sorted.sort(Comparator.comparingInt(Option::number)); // stable: repeated options keep order
        int last = 0;
        for (Option o : sorted) {
            int delta = o.number() - last;
            int len = o.value().length;
            out.write((nibble(delta) << 4) | nibble(len));
            extended(out, delta);
            extended(out, len);
            out.writeBytes(o.value());
            last = o.number();
        }
        if (payload.length > 0) {
            out.write(0xff);
            out.writeBytes(payload);
        }
        return out.toByteArray();
    }

    public static CoapMessage decode(byte[] b) {
        if (b.length < 4 || (b[0] & 0xc0) != 0x40) {
            throw new IllegalArgumentException("not a CoAP v1 message");
        }
        Type type = Type.values()[(b[0] >>> 4) & 3];
        int tkl = b[0] & 0x0f;
        if (tkl > 8 || 4 + tkl > b.length) {
            throw new IllegalArgumentException("bad token length");
        }
        int code = b[1] & 0xff;
        int mid = ((b[2] & 0xff) << 8) | (b[3] & 0xff);
        byte[] token = Arrays.copyOfRange(b, 4, 4 + tkl);
        int i = 4 + tkl;
        List<Option> options = new ArrayList<>();
        int number = 0;
        while (i < b.length && (b[i] & 0xff) != 0xff) {
            int head = b[i++] & 0xff;
            int[] delta = readExtended(b, i, head >>> 4);
            i = delta[1];
            int[] len = readExtended(b, i, head & 0x0f);
            i = len[1];
            if (i + len[0] > b.length) {
                throw new IllegalArgumentException("truncated option");
            }
            number += delta[0];
            options.add(new Option(number, Arrays.copyOfRange(b, i, i + len[0])));
            i += len[0];
        }
        byte[] payload = i < b.length ? Arrays.copyOfRange(b, i + 1, b.length) : new byte[0];
        return new CoapMessage(type, code, mid, token, List.copyOf(options), payload);
    }

    private static int nibble(int v) {
        return v < 13 ? v : v < 269 ? 13 : 14;
    }

    private static void extended(ByteArrayOutputStream out, int v) {
        if (v >= 269) {
            out.write((v - 269) >>> 8);
            out.write(v - 269);
        } else if (v >= 13) {
            out.write(v - 13);
        }
    }

    /** Returns {value, next index}. */
    private static int[] readExtended(byte[] b, int i, int nib) {
        int need = nib == 13 ? 1 : nib == 14 ? 2 : 0;
        if (i + need > b.length) {
            throw new IllegalArgumentException("truncated option header");
        }
        return switch (nib) {
            case 13 -> new int[] {(b[i] & 0xff) + 13, i + 1};
            case 14 -> new int[] {(((b[i] & 0xff) << 8) | (b[i + 1] & 0xff)) + 269, i + 2};
            case 15 -> throw new IllegalArgumentException("reserved option nibble 15");
            default -> new int[] {nib, i};
        };
    }
}
