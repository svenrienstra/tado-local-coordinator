package tadocoord.tado;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;

import java.util.List;
import org.junit.jupiter.api.Test;
import tadocoord.Eui;
import tadocoord.Golden;
import tadocoord.coap.CoapMessage;
import tadocoord.lowpan.Datagram;

/** Our join answers must match what freedo (and so the commercial bridge) sends, byte for byte. */
class TadoResourcesTest {
    private final Eui coordinator = Golden.eui("in.coord_eui");
    private final Eui joiner = Golden.eui("in.joiner_eui");

    @Test
    void pairRequestMatchesFreedo() {
        byte[] got = TadoResources.pairRequest(Golden.get("in.network_key"), 0x1234,
                Golden.get("in.pair_session"), coordinator, joiner);
        assertArrayEquals(Golden.get("pair.coap"), got);
    }

    @Test
    void authTokenAnswerMatchesFreedo() {
        CoapMessage request = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 2), 0x4321,
                new byte[] {(byte) 0xaa, (byte) 0xbb}, List.of(CoapMessage.path("auth"), CoapMessage.path("token")),
                new byte[0]);
        byte[] got = TadoResources.authTokenAnswer(request, Golden.get("in.session_id"),
                fromDevice(request), coordinator, joiner);
        assertArrayEquals(Golden.get("auth_token.coap"), got);
    }

    @Test
    void timeAnswerMatchesFreedo() {
        CoapMessage request = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 1), 0x5555,
                new byte[] {(byte) 0xcc}, List.of(CoapMessage.path("time")), new byte[0]);
        byte[] got = TadoResources.timeAnswer(request, 1786619383L, fromDevice(request), coordinator, joiner);
        assertArrayEquals(Golden.get("time.coap"), got);
    }

    @Test
    void routerAdvertMatchesFreedo() {
        assertArrayEquals(Golden.get("ra.packet"), TadoResources.routerAdvert(coordinator, new byte[4], new byte[4]));
    }

    private Datagram fromDevice(CoapMessage request) {
        byte[] wire = Datagram.udp(joiner, coordinator, Datagram.COAP_PORT, Datagram.COAP_PORT, request.encode());
        return Datagram.parse(wire, joiner, java.util.Optional.of(coordinator));
    }
}
