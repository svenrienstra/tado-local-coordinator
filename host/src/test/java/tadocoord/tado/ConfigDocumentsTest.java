package tadocoord.tado;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayOutputStream;
import java.util.HexFormat;
import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import tadocoord.Eui;
import tadocoord.coap.CoapMessage;
import tadocoord.lowpan.Datagram;

class ConfigDocumentsTest {
    private final Eui coordinator = Eui.ofHex("1e34217520c51b00");
    private final Eui sensor = Eui.ofHex("5b4b713107c51b00");

    @Test
    void zoneUriFlipsTheUniversalLocalBitAsFreedoDoes() {
        // freedo's example (tado/build_zone_config.go zoneConfigPathWire).
        assertEquals("coap://[fe80:0:0:0:21b:c507:face:a602]/z/", ConfigDocuments.zoneUri(Eui.ofHex("02a6cefa07c51b00")));
    }

    @Test
    void deviceConfigCarriesTheHomeIdAndZoneRolesAndNothingTheDeviceKeeps() {
        byte[] doc = ConfigDocuments.deviceConfig(0x12345678L, List.of(new ConfigDocuments.ZoneRole(0x0d, 11)));

        assertEquals("014301000" + "15c041234567802b30100015e020d0b0158020200", HexFormat.of().formatHex(doc));
        assertThrows(IllegalArgumentException.class, () -> ConfigDocuments.deviceConfig(0, List.of()));
    }

    @Test
    void servesADocumentIn64ByteBlocksThatEachFitOneFrame() {
        byte[] doc = ConfigDocuments.zoneWithOwner(sensor);
        assertTrue(doc.length > 128, "needs several blocks");

        ByteArrayOutputStream joined = new ByteArrayOutputStream();
        for (int num = 0; ; num++) {
            // The device asks for 128-byte blocks; block n of 64 follows its first answer.
            int asked = num == 0 ? 0x03 : num << 4 | 2;
            CoapMessage answer = answerTo(get(asked, null), doc);
            assertEquals(CoapMessage.code(2, 5), answer.code());
            int block = unsigned(option(answer, ConfigDocuments.BLOCK2));
            assertEquals(num, block >>> 4);
            assertEquals(2, block & 7);
            joined.writeBytes(answer.payload());
            if ((block & 8) == 0) {
                break;
            }
        }
        assertArrayEquals(doc, joined.toByteArray());
    }

    @Test
    void answers203WhenTheDeviceAlreadyHoldsTheDocument() {
        byte[] doc = ConfigDocuments.deviceConfig(42, List.of());
        CoapMessage answer = answerTo(get(0x03, ConfigDocuments.etag(doc)), doc);

        assertEquals(CoapMessage.code(2, 3), answer.code());
        assertEquals(0, answer.payload().length);
        assertArrayEquals(ConfigDocuments.etag(doc), option(answer, ConfigDocuments.ETAG));
    }

    private CoapMessage get(int block2, byte[] etag) {
        List<CoapMessage.Option> options = new java.util.ArrayList<>();
        if (etag != null) {
            options.add(new CoapMessage.Option(ConfigDocuments.ETAG, etag));
        }
        options.add(CoapMessage.path("z"));
        options.add(new CoapMessage.Option(ConfigDocuments.BLOCK2, new byte[] {(byte) block2}));
        return new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 1), 0x1234, new byte[0], options, new byte[0]);
    }

    private CoapMessage answerTo(CoapMessage request, byte[] doc) {
        Datagram from = Datagram.parse(Datagram.udp(sensor, coordinator, 5683, 5683, request.encode()), sensor,
                Optional.of(coordinator));
        byte[] packet = ConfigDocuments.blockAnswer(request, doc, from, coordinator, sensor);
        assertTrue(packet.length <= 97, "a block must fit one unicast frame: " + packet.length);
        return CoapMessage.decode(Datagram.parse(packet, coordinator, Optional.of(sensor)).payload());
    }

    private static byte[] option(CoapMessage m, int number) {
        return m.options().stream().filter(o -> o.number() == number).findFirst().orElseThrow().value();
    }

    private static int unsigned(byte[] v) {
        int x = 0;
        for (byte b : v) {
            x = x << 8 | (b & 0xff);
        }
        return x;
    }
}
