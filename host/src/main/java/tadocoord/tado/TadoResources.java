package tadocoord.tado;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import tadocoord.Eui;
import tadocoord.coap.CoapMessage;
import tadocoord.coap.CoapMessage.Option;
import tadocoord.coap.CoapMessage.Type;
import tadocoord.lowpan.Datagram;

/**
 * The coordinator side of the join exchange (docs/JOIN.md), byte-compatible with the commercial
 * bridge as freedo reconstructs it. Each method returns a 6LoWPAN datagram ready for {@code TX}.
 */
public final class TadoResources {
    private static final int CONTENT_FORMAT_OCTETS = 42;
    private static final int PAIR_KEY_TAG = 0x12; // plaintext network key (0x07 is cloud-KEK wrapped)
    private static final int SESSION_LIFETIME_MIN = 1440;
    private static final int PROTOBUF_FIXED32_FIELD1 = 0x0d;

    private TadoResources() {}

    /**
     * The {@code d/pair} CON POST that hands a joiner our network key, sent under the pairing
     * key. {@code session} is the single-use 8-byte value of option 2048, new on each request.
     */
    public static byte[] pairRequest(byte[] networkKey, int mid, byte[] session, Eui coordinator, Eui joiner) {
        if (networkKey.length != 16 || session.length != 8) {
            throw new IllegalArgumentException("network key must be 16 bytes and session 8");
        }
        byte[] tlv = new byte[18];
        tlv[0] = PAIR_KEY_TAG;
        tlv[1] = 16;
        System.arraycopy(networkKey, 0, tlv, 2, 16);
        CoapMessage m = new CoapMessage(Type.CON, CoapMessage.code(0, 2), mid, new byte[0],
                List.of(CoapMessage.path("d"), CoapMessage.path("pair"),
                        new Option(CoapMessage.CONTENT_FORMAT, new byte[] {CONTENT_FORMAT_OCTETS}),
                        new Option(CoapMessage.SESSION, session)),
                tlv);
        return Datagram.udp(coordinator, joiner, Datagram.COAP_PORT, Datagram.COAP_PORT, m.encode());
    }

    /**
     * The 2.05 to {@code auth/token}: session id (TLV 025e) and lifetime (025f), no options.
     * TLV 0261 (the permanent token) is never sent: the device decrypts it with a per-device key
     * we do not hold and would store garbage.
     */
    public static byte[] authTokenAnswer(CoapMessage request, byte[] sessionId, Datagram from,
            Eui coordinator, Eui device) {
        if (sessionId.length < 4 || sessionId.length > 8) {
            throw new IllegalArgumentException("session id must be 4..8 bytes");
        }
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        tlv16(body, 0x025e, sessionId);
        tlv16(body, 0x025f, new byte[] {(byte) (SESSION_LIFETIME_MIN >>> 8), (byte) SESSION_LIFETIME_MIN});
        return answer(request, CoapMessage.code(2, 5), body.toByteArray(), from, coordinator, device);
    }

    /**
     * The 2.05 to {@code time}: protobuf field 1 fixed32 = Unix seconds, no options. The device
     * drops an answer more than 5 s after its request, so it must not wait for a rendezvous.
     */
    public static byte[] timeAnswer(CoapMessage request, long unixSeconds, Datagram from,
            Eui coordinator, Eui device) {
        byte[] body = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
                .put((byte) PROTOBUF_FIXED32_FIELD1).putInt((int) unixSeconds).array();
        return answer(request, CoapMessage.code(2, 5), body, from, coordinator, device);
    }

    /**
     * Router Advertisement to ff02::1 as the bridge sends it: hop limit 64, O flag, lifetime
     * 1800 s, SLLAO, MTU 1500, and option 25 with the IPv4 netmask, gateway and DNS.
     */
    public static byte[] routerAdvert(Eui coordinator, byte[] gatewayV4, byte[] dnsV4) {
        ByteArrayOutputStream m = new ByteArrayOutputStream(80);
        m.writeBytes(new byte[] {(byte) 134, 0, 0, 0, 64, 0x40, 0x07, 0x08});
        m.writeBytes(new byte[8]); // reachable time, retransmit timer
        m.writeBytes(new byte[] {1, 2});
        m.writeBytes(coordinator.bigEndian());
        m.writeBytes(new byte[6]);
        m.writeBytes(new byte[] {5, 1, 0, 0, 0, 0, 0x05, (byte) 0xdc});
        m.writeBytes(new byte[] {25, 5, 0, 0, (byte) 0xff, (byte) 0xff, (byte) 0xff, (byte) 0xff});
        m.writeBytes(ipv4Mapped(gatewayV4));
        m.writeBytes(ipv4Mapped(dnsV4));
        return Datagram.icmpv6ToLinkMulticast(coordinator, 1, m.toByteArray());
    }

    /** The empty ACK (code 0.00) that confirms a CON response, echoing its MID. */
    public static byte[] emptyAck(CoapMessage con, Datagram from, Eui coordinator, Eui device) {
        CoapMessage m = new CoapMessage(Type.ACK, 0, con.mid(), new byte[0], List.of(), new byte[0]);
        return Datagram.udp(coordinator, device, from.dstPort(), from.srcPort(), m.encode());
    }

    /** The bridge's default answer to a PUT or POST: a piggybacked 2.04 with no body. */
    public static byte[] changedAnswer(CoapMessage request, Datagram from, Eui coordinator, Eui device) {
        return answer(request, CoapMessage.code(2, 4), new byte[0], from, coordinator, device);
    }

    /** A piggybacked ACK echoing MID and token, ports swapped, with no options. */
    private static byte[] answer(CoapMessage request, int code, byte[] body, Datagram from,
            Eui coordinator, Eui device) {
        CoapMessage m = new CoapMessage(Type.ACK, code, request.mid(), request.token(), List.of(), body);
        return Datagram.udp(coordinator, device, from.dstPort(), from.srcPort(), m.encode());
    }

    private static void tlv16(ByteArrayOutputStream out, int tag, byte[] value) {
        out.write(tag >>> 8);
        out.write(tag);
        out.write(value.length);
        out.writeBytes(value);
    }

    private static byte[] ipv4Mapped(byte[] v4) {
        if (v4.length != 4) {
            throw new IllegalArgumentException("IPv4 address must be 4 bytes");
        }
        byte[] a = new byte[16];
        a[10] = (byte) 0xff;
        a[11] = (byte) 0xff;
        System.arraycopy(v4, 0, a, 12, 4);
        return a;
    }
}
