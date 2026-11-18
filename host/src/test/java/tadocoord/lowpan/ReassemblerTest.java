package tadocoord.lowpan;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import tadocoord.Eui;
import tadocoord.coap.CoapMessage;

class ReassemblerTest {
    /**
     * A wireless temperature sensor's {@code PUT d/<serial>/fw/state} right after joining
     * (capture of 2026-11-17): 139 uncompressed bytes, FRAG1 with 80 CoAP bytes, FRAGN at 128.
     */
    private static final byte[] FRAG1 = HexFormat.of().parseHex(
            "c08b00007e33f016330fa5a33c4003a302b1640c535532393232303830323536026677057374617465"
                    + "112ae806e7c4358ade308b0efdff01a00108003a0235c1003b010e003502124100390235c100"
                    + "36010a003c010e02100731353536");
    private static final byte[] FRAGN = HexFormat.of().parseHex("e08b00001063356501800101014c0101");

    private final Eui sensor = Eui.ofHex("5b4b713107c51b00");
    private final Eui coordinator = Eui.ofHex("1e34217520c51b00");
    private final Instant t0 = Instant.parse("2026-11-17T09:50:42Z");
    private final Reassembler r = new Reassembler();

    @Test
    void reassemblesTheSensorsFragmentedStateReport() {
        assertTrue(r.add(sensor, FRAG1, t0).isEmpty());
        byte[] whole = r.add(sensor, FRAGN, t0.plusMillis(24)).orElseThrow();

        Datagram d = Datagram.parse(whole, sensor, Optional.of(coordinator));
        CoapMessage m = CoapMessage.decode(d.payload());
        assertEquals("d/SU2922080256/fw/state", m.path());
        assertEquals(CoapMessage.Type.CON, m.type());
        assertEquals(91, d.payload().length); // 139 - 48 bytes of uncompressed IPv6 + UDP headers
    }

    @Test
    void acceptsFragmentsOutOfOrderAndIgnoresMacRetransmissions() {
        assertTrue(r.add(sensor, FRAGN, t0).isEmpty());
        assertTrue(r.add(sensor, FRAGN, t0).isEmpty());
        byte[] whole = r.add(sensor, FRAG1, t0).orElseThrow();

        Reassembler fresh = new Reassembler();
        fresh.add(sensor, FRAG1, t0);
        assertArrayEquals(fresh.add(sensor, FRAGN, t0).orElseThrow(), whole);
    }

    @Test
    void dropsADatagramWhoseLastFragmentComesAfterTheTimeout() {
        r.add(sensor, FRAG1, t0);

        assertTrue(r.add(sensor, FRAGN, t0.plus(Reassembler.TIMEOUT)).isEmpty());
    }

    @Test
    void keepsDatagramsOfDifferentSourcesApart() {
        Eui other = Eui.ofHex("0b992d3107c51b00");
        r.add(sensor, FRAG1, t0);

        assertTrue(r.add(other, FRAGN, t0).isEmpty());
        assertTrue(r.add(sensor, FRAGN, t0).isPresent());
    }

    @Test
    void refusesAnOverlappingFragmentAndDropsItsDatagram() {
        byte[] overlapping = FRAGN.clone();
        overlapping[4] = 0x0f; // offset 120, inside the 128 bytes FRAG1 stands for
        r.add(sensor, FRAG1, t0);

        assertThrows(IllegalArgumentException.class, () -> r.add(sensor, overlapping, t0));
        assertTrue(r.add(sensor, FRAGN, t0).isEmpty()); // FRAG1 went with the dropped datagram
    }

    @Test
    void reassemblesADatagramWhoseUdpHeaderIsCarriedInline() {
        // IPHC with next header 17 inline (no NHC): the 8-byte UDP header travels as is.
        byte[] payload = new byte[80];
        Arrays.fill(payload, (byte) 0x5a);
        byte[] head = HexFormat.of().parseHex("7b3311" + "1633163300580000");
        int size = 40 + 8 + payload.length; // 128
        byte[] frag1 = concat(new byte[] {(byte) 0xc0, (byte) size, 0, 7}, head, Arrays.copyOf(payload, 32));
        byte[] fragN = concat(new byte[] {(byte) 0xe0, (byte) size, 0, 7, (byte) ((48 + 32) / 8)},
                Arrays.copyOfRange(payload, 32, payload.length));

        assertTrue(r.add(sensor, frag1, t0).isEmpty());
        Datagram d = Datagram.parse(r.add(sensor, fragN, t0).orElseThrow(), sensor, Optional.of(coordinator));

        assertEquals(5683, d.dstPort());
        assertArrayEquals(payload, d.payload());
    }

    private static byte[] concat(byte[]... parts) {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        for (byte[] p : parts) {
            out.writeBytes(p);
        }
        return out.toByteArray();
    }

    @Test
    void passesAnUnfragmentedDatagramThrough() {
        byte[] p = HexFormat.of().parseHex("7e33f016330fa5a33c40");

        assertArrayEquals(p, r.add(sensor, p, t0).orElseThrow());
    }
}
