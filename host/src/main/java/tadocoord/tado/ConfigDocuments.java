package tadocoord.tado;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalInt;
import tadocoord.Eui;
import tadocoord.coap.CoapMessage;
import tadocoord.coap.CoapMessage.Option;
import tadocoord.coap.CoapMessage.Type;
import tadocoord.lowpan.Datagram;

/**
 * The documents a device reads from its coordinator: its own configuration ({@code GET
 * d/<serial>/config}) and its zone's ({@code GET h/<home>/z/<n>/config}), with the Block2
 * answers that carry them. Shapes and tags as freedo serves them to valves and the wall
 * thermostat (docs/SENSOR.md); TLVs are a 2-byte tag, a 1-byte length and a big-endian value.
 */
public final class ConfigDocuments {
    static final int ETAG = 4;
    static final int BLOCK2 = 23;
    /**
     * 64-byte blocks: a 128-byte block does not fit one frame (97 bytes of packet) and would
     * need 6LoWPAN fragmentation. A server may answer with a smaller block than asked for.
     */
    static final int MAX_SZX = 2;

    /** Role byte of a {@code 015e} record: an elected owner (bit 3) of its own zone. */
    public static final int ROLE_OWNER = 0x0d;
    public static final int ROLE_MEMBER = 0x05;
    /** {@code 6240}: 0 no overlay, 1 until the next automatic change, 2 manual, 3 timer. */
    public static final int OVERLAY_MANUAL = 2;

    private ConfigDocuments() {}

    /** One {@code 015e} record: the device's role in a zone. */
    public record ZoneRole(int role, int zone) {}

    /**
     * A device's configuration: trace off, our home id, no offline schedule, its zone roles and
     * notification on remote overlay changes. It leaves out what a device keeps of its own: the
     * interface module id {@code 015d}, the offset {@code 0140}, and the valve-only {@code 021a}
     * and {@code 0149}. Every value fits the device's 8-byte record buffer.
     */
    public static byte[] deviceConfig(long homeId, List<ZoneRole> zones) {
        if (homeId <= 0 || homeId > 0xffffffffL) {
            throw new IllegalArgumentException("home id must be 1..2^32-1: a device applies nothing with 0");
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        tlv(out, 0x0143, new byte[] {0});
        tlv(out, 0x015c, new byte[] {(byte) (homeId >>> 24), (byte) (homeId >>> 16), (byte) (homeId >>> 8), (byte) homeId});
        tlv(out, 0x02b3, new byte[] {0});
        for (ZoneRole z : zones) {
            tlv(out, 0x015e, new byte[] {(byte) z.role(), (byte) z.zone()});
        }
        tlv(out, 0x0158, new byte[] {0x02, 0x00});
        return out.toByteArray();
    }

    /**
     * A heating zone with one device as owner and measuring endpoint, no circuit, freedo's PID
     * defaults and open-window detection on.
     */
    public static byte[] zoneWithOwner(Eui owner) {
        String o = zoneUri(owner);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        tlv(out, 0x6020, new byte[] {1}); // heating
        tlv(out, 0x63e0, new byte[] {1});
        tlv(out, 0x63a0, ascii(o + "s"));
        tlv(out, 0x8400, ascii(o + "p"));
        tlv(out, 0x8000, ascii(o + "s"));
        tlv(out, 0x6040, ascii(o + "p"));
        tlv(out, 0x6080, u16(1000)); // Kp
        tlv(out, 0x60a0, u16(500)); // Ki
        tlv(out, 0x60c0, u16(1500)); // Kd
        tlv(out, 0x60e0, new byte[] {1}); // open-window detection
        tlv(out, 0x62c0, u16(900)); // open-window time, s
        tlv(out, 0x6380, u16(900));
        return out.toByteArray();
    }

    /**
     * A zone's state as a device reads it ({@code GET h/<home>/z/<n>/s}) and as the coordinator
     * writes it ({@code PUT z/s?id=<n>}): at home, a heating zone, on, following {@code setpoint}
     * (centi-degrees C), no open window. With an {@code overlay} setpoint, a manual overlay
     * ({@code 6240}=2, {@code 6260}=1, {@code 6280}) on top; without, {@code 6240}=0, which ends
     * one the device holds. The device keeps any tag a body leaves out.
     */
    public static byte[] zoneState(int setpoint, OptionalInt overlay) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        tlv(out, 0x6160, new byte[] {1}); // home
        tlv(out, 0x6180, new byte[] {0});
        tlv(out, 0x6020, new byte[] {1}); // heating
        tlv(out, 0x61e0, new byte[] {1}); // on
        tlv(out, 0x6200, u16(setpoint));
        if (overlay.isPresent()) {
            tlv(out, 0x6240, new byte[] {OVERLAY_MANUAL});
            tlv(out, 0x6260, new byte[] {1}); // on
            tlv(out, 0x6280, u16(overlay.getAsInt()));
        } else {
            tlv(out, 0x6240, new byte[] {0});
        }
        tlv(out, 0x62e0, new byte[] {0}); // window closed
        return out.toByteArray();
    }

    public static byte[] zoneState(int setpoint) {
        return zoneState(setpoint, OptionalInt.empty());
    }

    /** A TLV body by tag; a repeated tag keeps its last value. */
    public static Map<Integer, byte[]> tlvs(byte[] body) {
        Map<Integer, byte[]> out = new HashMap<>();
        for (int i = 0; i + 3 <= body.length; ) {
            int tag = (body[i] & 0xff) << 8 | (body[i + 1] & 0xff);
            int len = body[i + 2] & 0xff;
            if (i + 3 + len > body.length) {
                throw new IllegalArgumentException("TLV %04x runs past the body".formatted(tag));
            }
            out.put(tag, Arrays.copyOfRange(body, i + 3, i + 3 + len));
            i += 3 + len;
        }
        return out;
    }

    /**
     * {@code coap://[fe80:0:0:0:..]/z/}: the device's link-local address, its EUI-64 with the
     * U/L bit flipped, in lowercase hex groups without leading zeros.
     */
    public static String zoneUri(Eui device) {
        byte[] b = device.bigEndian().clone();
        b[0] ^= 0x02;
        StringBuilder s = new StringBuilder("coap://[fe80:0:0:0");
        for (int i = 0; i < 8; i += 2) {
            s.append(':').append(Integer.toHexString(((b[i] & 0xff) << 8) | (b[i + 1] & 0xff)));
        }
        return s.append("]/z/").toString();
    }

    /**
     * The document's ETag, FNV-1a 64 big-endian: the device only compares it, and the gateway
     * computes the same in C (gateway/core/tado.c).
     */
    public static byte[] etag(byte[] document) {
        long h = 0xcbf29ce484222325L;
        for (byte b : document) {
            h ^= b & 0xff;
            h *= 0x100000001b3L;
        }
        return java.nio.ByteBuffer.allocate(8).putLong(h).array();
    }

    /**
     * The answer to a GET of {@code document}: {@code 2.03} with the ETag when the request holds
     * it for block 0, else {@code 2.05} with the ETag and the block the request asks for, at most
     * 64 bytes. No Content-Format, as the bridge's.
     */
    public static byte[] blockAnswer(CoapMessage request, byte[] document, Datagram from, Eui coordinator,
            Eui device) {
        byte[] tag = etag(document);
        int block = request.options().stream().filter(o -> o.number() == BLOCK2).findFirst()
                .map(o -> unsigned(o.value())).orElse(MAX_SZX);
        int szx = Math.min(block & 7, MAX_SZX);
        // RFC 7959 2.4: in smaller blocks, the asked block starts at our block NUM << (size step).
        int num = (block >>> 4) << ((block & 7) - szx);
        Optional<byte[]> held = request.options().stream().filter(o -> o.number() == ETAG)
                .map(Option::value).findFirst();
        List<Option> options = new ArrayList<>();
        options.add(new Option(ETAG, tag));
        int code;
        byte[] body;
        if (num == 0 && held.isPresent() && Arrays.equals(held.get(), tag)) {
            code = CoapMessage.code(2, 3);
            body = new byte[0];
            options.add(new Option(BLOCK2, blockValue(0, false, szx)));
        } else {
            int size = 16 << szx;
            int from0 = Math.min(num * size, document.length);
            int to = Math.min(from0 + size, document.length);
            code = CoapMessage.code(2, 5);
            body = Arrays.copyOfRange(document, from0, to);
            options.add(new Option(BLOCK2, blockValue(num, to < document.length, szx)));
        }
        CoapMessage m = new CoapMessage(Type.ACK, code, request.mid(), request.token(), options, body);
        return Datagram.udp(coordinator, device, from.dstPort(), from.srcPort(), m.encode());
    }

    static byte[] blockValue(int num, boolean more, int szx) {
        int v = num << 4 | (more ? 8 : 0) | szx;
        if (v == 0) {
            return new byte[0];
        }
        return v < 0x100 ? new byte[] {(byte) v} : v < 0x10000 ? new byte[] {(byte) (v >>> 8), (byte) v}
                : new byte[] {(byte) (v >>> 16), (byte) (v >>> 8), (byte) v};
    }

    public static int unsigned(byte[] v) {
        int x = 0;
        for (byte b : v) {
            x = x << 8 | (b & 0xff);
        }
        return x;
    }

    private static void tlv(ByteArrayOutputStream out, int tag, byte[] value) {
        if (value.length > 0xff) {
            throw new IllegalArgumentException("TLV value over 255 bytes");
        }
        out.write(tag >>> 8);
        out.write(tag);
        out.write(value.length);
        out.writeBytes(value);
    }

    private static byte[] u16(int v) {
        return new byte[] {(byte) (v >>> 8), (byte) v};
    }

    private static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.US_ASCII);
    }
}
