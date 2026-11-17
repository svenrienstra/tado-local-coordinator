package tadocoord.lowpan;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
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
    void passesAnUnfragmentedDatagramThrough() {
        byte[] p = HexFormat.of().parseHex("7e33f016330fa5a33c40");

        assertArrayEquals(p, r.add(sensor, p, t0).orElseThrow());
    }
}
