package tadocoord.lowpan;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Arrays;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import tadocoord.Eui;
import tadocoord.Golden;
import tadocoord.coap.CoapMessage;

class DatagramTest {
    private final Eui coordinator = Golden.eui("in.coord_eui");
    private final Eui joiner = Golden.eui("in.joiner_eui");

    @Test
    void parsesTheValvesPairAnswer() {
        Datagram d = Datagram.parse(Golden.get("pair_answer.coap"), joiner, Optional.of(coordinator));

        assertEquals(Ipv6.NH_UDP, d.nextHeader());
        assertEquals(5683, d.srcPort());
        assertEquals(5683, d.dstPort());
        assertArrayEquals(joiner.linkLocal(), d.src().orElseThrow());
        assertArrayEquals(coordinator.linkLocal(), d.dst().orElseThrow());
        CoapMessage m = CoapMessage.decode(d.payload());
        assertEquals(CoapMessage.Type.ACK, m.type());
        assertEquals(CoapMessage.code(2, 4), m.code());
        assertEquals(0x1234, m.mid());
    }

    @Test
    void parsesABroadcastIcmpv6ToAllNodes() {
        Datagram d = Datagram.parse(Golden.get("ra.packet"), coordinator, Optional.empty());

        assertEquals(Ipv6.NH_ICMPV6, d.nextHeader());
        assertArrayEquals(Ipv6.linkScopeMulticast(1), d.dst().orElseThrow());
        assertEquals(134, d.payload()[0] & 0xff);
    }

    @Test
    void decodesTheOptionsOfDPairIncludingTheSessionOption() {
        Datagram d = Datagram.parse(Golden.get("pair.coap"), coordinator, Optional.of(joiner));
        CoapMessage m = CoapMessage.decode(d.payload());

        assertEquals("d/pair", m.path());
        assertTrue(m.options().stream().anyMatch(o -> o.number() == CoapMessage.SESSION
                && Arrays.equals(o.value(), Golden.get("in.pair_session"))));
        assertArrayEquals(Golden.get("pair.coap"), Datagram.udp(coordinator, joiner, 5683, 5683, m.encode()));
    }

    @Test
    void parsesAStatefulMulticastDestinationAsSixInlineBytes() {
        // IPHC 7e 3c: NH compressed, M=1 DAC=1 DAM=00 (48 bits inline); then NHC UDP with inline ports.
        byte[] p = {0x7e, 0x3c, 1, 2, 3, 4, 5, 6, (byte) 0xf0, 0x16, 0x33, 0x16, 0x34, 0, 0, 0x42};
        Datagram d = Datagram.parse(p, joiner, Optional.empty());

        assertEquals(5683, d.srcPort());
        assertEquals(5684, d.dstPort());
        assertArrayEquals(new byte[] {0x42}, d.payload());
        assertTrue(d.dst().isEmpty());
    }

    @Test
    void refusesATruncatedDatagram() {
        byte[] p = Golden.get("pair_answer.coap");
        assertThrows(IllegalArgumentException.class,
                () -> Datagram.parse(Arrays.copyOf(p, 5), joiner, Optional.of(coordinator)));
    }
}
