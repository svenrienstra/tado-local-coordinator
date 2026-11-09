package tadocoord.link;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.List;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import tadocoord.Golden;
import tadocoord.Hex;

class LinkCaptureTest {

    @Test
    void neverWritesTheNetworkKeyFromConfigOrDPair(@TempDir Path dir) throws Exception {
        byte[] key = Golden.get("in.network_key");
        byte[] config = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN)
                .put(Golden.get("in.coord_eui")).put(key).putInt(1).putInt(868_300).array();
        // TX of d/pair: flags, destination, then the datagram carrying the key in TLV 0x12.
        byte[] pairTx = ByteBuffer.allocate(9 + Golden.get("pair.coap").length)
                .put((byte) 0x11).put(Golden.get("in.joiner_eui")).put(Golden.get("pair.coap")).array();
        Path file = dir.resolve("captures/link.log");
        try (LinkCapture capture = new LinkCapture(file, () -> Instant.EPOCH, List.of(key))) {
            capture.frame(true, 1, HostLink.CONFIG, config);
            capture.frame(true, 2, HostLink.TX, pairTx);
        }

        String written = Files.readString(file);
        assertTrue(written.contains("CONFIG (key redacted)"));
        assertTrue(written.contains("TX (key redacted)"));
        assertFalse(written.contains(Hex.of(key)));
    }

    @Test
    void namesRepliesAndDecodesTheMacHeaderOfUnverifiedFrames() {
        byte[] frame = Golden.get("pair_answer.frame");
        byte[] event = ByteBuffer.allocate(5 + frame.length).put((byte) -61).putInt(0).put(frame).array();

        String rx = LinkCapture.line("t", false, 7, Event.RX_UNVERIFIED, event, List.of());
        String reply = LinkCapture.line("t", false, 3, HostLink.PING | HostLink.REPLY_FLAG, new byte[] {0}, List.of());

        assertTrue(rx.contains("RX_UNVERIFIED rssi=-61 data dst=09a1cefa07c51b00 src=0bb1cefa07c51b00"), rx);
        assertTrue(reply.contains("PING_REPLY"), reply);
    }

    @Test
    void recognisesEveryFrameKindOfTheJoin() {
        for (var c : List.of(
                List.of("beacon_request.frame", "beacon-request src=0bb1cefa07c51b00"),
                List.of("enh_beacon.frame", "enhanced-beacon"),
                List.of("enh_ack.frame", "enh-ack dst=0bb1cefa07c51b00"),
                List.of("wakeup.bcast_3125", "wake-up"),
                List.of("ra.frame_pairing", "broadcast src=09a1cefa07c51b00"))) {
            assertEquals(c.get(1), LinkCapture.describeMac(Golden.get(c.get(0))), c.get(0));
        }
    }
}
