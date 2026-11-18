package tadocoord.join;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.time.InstantSource;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Optional;
import java.util.SplittableRandom;
import org.junit.jupiter.api.Test;
import tadocoord.Eui;
import tadocoord.Golden;
import tadocoord.coap.CoapMessage;
import tadocoord.link.Event;
import tadocoord.link.Radio;
import tadocoord.link.Radio.TxRequest;
import tadocoord.lowpan.Datagram;

class JoinSessionTest {
    private final Eui coordinator = Golden.eui("in.coord_eui");
    private final Eui joiner = Golden.eui("in.joiner_eui");
    private final byte[] networkKey = Golden.get("in.network_key");

    private final List<TxRequest> sent = new ArrayList<>();
    private final Radio radio = r -> {
        sent.add(r);
        return new Radio.TxAccepted(sent.size(), sent.size(), sent.size());
    };
    private Instant now = Instant.parse("2026-11-09T12:00:00Z");
    private final InstantSource clock = () -> now;
    private final JoinSession session = new JoinSession(coordinator, networkKey, radio, clock,
            new SplittableRandom(1), msg -> { });

    @Test
    void walksAValveFromBeaconToJoined() throws Exception {
        session.onEvent(new Event.BeaconAnswered(joiner, 16, -40, 0x59));
        session.onEvent(routerSolicitation());

        // The RA goes twice: network key with Frame Pending, then the pairing key; both behind full trains.
        assertEquals(2, sent.size());
        assertEquals(TxRequest.FRAME_PENDING | TxRequest.TRAIN_FULL, sent.get(0).flags());
        assertTrue(sent.get(0).dst().isEmpty());
        assertEquals(TxRequest.PAIRING_KEY | TxRequest.TRAIN_FULL, sent.get(1).flags());
        assertTrue(sent.get(1).dst().isEmpty());
        assertArrayEquals(sent.get(0).packet(), sent.get(1).packet());

        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));

        TxRequest pair = sent.get(2);
        assertEquals(TxRequest.PAIRING_KEY | TxRequest.TRAIN_FULL, pair.flags());
        assertEquals(Optional.of(joiner), pair.dst());
        CoapMessage m = coap(pair);
        assertEquals("d/pair", m.path());
        assertArrayEquals(networkKey, Arrays.copyOfRange(m.payload(), 2, 18));
        assertEquals(JoinSession.State.PAIRING, session.state());

        session.onEvent(new Event.TxDone(3, Event.TxDone.Result.ACKED));
        session.onEvent(fromJoiner(false, ack(m.mid(), CoapMessage.code(2, 4))));

        assertEquals(JoinSession.State.JOINED, session.state());
    }

    @Test
    void aSolicitationWithinThreeSecondsOfTheLastAdvertisementGetsNone() throws Exception {
        session.onEvent(routerSolicitation());
        session.onEvent(routerSolicitation()); // while the first pair is still on air
        assertEquals(2, sent.size());

        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));
        int afterPair = sent.size(); // the RA ended, so d/pair followed
        now = now.plusSeconds(2);
        session.onEvent(routerSolicitation());
        assertEquals(afterPair, sent.size());

        now = now.plusSeconds(2);
        session.onEvent(routerSolicitation());
        assertEquals(afterPair + 2, sent.size());
    }

    @Test
    void retriesDPairWithANewMidAndGivesUpAfterTheLastAttempt() throws Exception {
        session.onEvent(new Event.BeaconAnswered(joiner, 16, -40, 0x59));
        session.onEvent(routerSolicitation());
        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));
        List<Integer> mids = new ArrayList<>();

        for (int attempt = 0; attempt < JoinSession.PAIR_ATTEMPTS; attempt++) {
            TxRequest pair = sent.get(sent.size() - 1);
            mids.add(coap(pair).mid());
            session.onEvent(new Event.TxDone(sent.size(), Event.TxDone.Result.NO_ACK));
            now = now.plus(JoinSession.PAIR_ANSWER_TIMEOUT);
            session.tick();
        }

        assertEquals(JoinSession.State.FAILED, session.state());
        assertEquals(JoinSession.PAIR_ATTEMPTS, mids.stream().distinct().count());
    }

    @Test
    void aLate204ToAnEarlierAttemptStillCountsAsJoined() throws Exception {
        session.onEvent(new Event.BeaconAnswered(joiner, 16, -40, 0x59));
        session.onEvent(routerSolicitation());
        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));
        int firstMid = coap(sent.get(2)).mid();
        session.onEvent(new Event.TxDone(3, Event.TxDone.Result.NO_ACK));
        now = now.plus(JoinSession.PAIR_ANSWER_TIMEOUT);
        session.tick(); // attempt 2 goes out with a new MID
        assertNotEquals(firstMid, coap(sent.get(3)).mid());

        session.onEvent(fromJoiner(false, ack(firstMid, CoapMessage.code(2, 4))));

        assertEquals(JoinSession.State.JOINED, session.state());
    }

    @Test
    void lostTxDoneEventsDoNotStallTheJoin() throws Exception {
        session.onEvent(new Event.BeaconAnswered(joiner, 16, -40, 0x59));
        session.onEvent(routerSolicitation());
        assertEquals(2, sent.size());

        now = now.plus(JoinSession.TX_DONE_TIMEOUT); // the RA's TX_DONE never comes
        session.tick();
        assertEquals(3, sent.size(), "d/pair follows the RA anyway");

        now = now.plus(JoinSession.TX_DONE_TIMEOUT); // nor does d/pair's
        session.tick();
        now = now.plus(JoinSession.PAIR_ANSWER_TIMEOUT);
        session.tick();
        assertEquals(4, sent.size(), "d/pair is retried");
    }

    @Test
    void anEmptyAckThenASeparate204Joins() throws Exception {
        session.onEvent(new Event.BeaconAnswered(joiner, 16, -40, 0x59));
        session.onEvent(routerSolicitation());
        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));
        int mid = coap(sent.get(2)).mid();

        session.onEvent(fromJoiner(false, ack(mid, 0)));
        assertEquals(JoinSession.State.PAIRING, session.state());

        CoapMessage separate = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(2, 4), 0x7777,
                new byte[0], List.of(), new byte[0]);
        session.onEvent(fromJoiner(false, separate));

        assertEquals(JoinSession.State.JOINED, session.state());
        CoapMessage confirm = coap(sent.get(3));
        assertEquals(CoapMessage.Type.ACK, confirm.type());
        assertEquals(0, confirm.code());
        assertEquals(0x7777, confirm.mid());
    }

    @Test
    void aJoinerLearnedWithinTheAdvertisementGapStillGetsDPair() throws Exception {
        session.onEvent(new Event.Rx(false, true, false, -50, 1, Golden.eui("in.coord_eui"), 1, 0,
                new byte[] {0x7b, 0x3b, 58, 0x02, (byte) 133, 0, 0, 0, 0, 0, 0, 0})); // RS from another node
        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));
        now = now.plusSeconds(1);

        session.onEvent(routerSolicitation()); // the joiner, under the pairing key, 1 s after the RA

        assertEquals(3, sent.size());
        assertEquals("d/pair", coap(sent.get(2)).path());
    }

    @Test
    void anAnswerUnderThePairingKeyMeansTheValveDidNotAdoptOurKey() throws Exception {
        session.onEvent(new Event.BeaconAnswered(joiner, 16, -40, 0x59));
        session.onEvent(routerSolicitation());
        session.onEvent(new Event.TxDone(2, Event.TxDone.Result.SENT));
        int mid = coap(sent.get(2)).mid();

        session.onEvent(fromJoiner(true, ack(mid, CoapMessage.code(2, 4))));

        assertEquals(JoinSession.State.FAILED, session.state());
    }

    @Test
    void servesASessionAndTheTimeAfterTheJoin() throws Exception {
        CoapMessage auth = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 2), 0x0101,
                new byte[] {1}, List.of(CoapMessage.path("auth"), CoapMessage.path("token")), new byte[0]);
        session.onEvent(fromJoiner(false, auth));

        CoapMessage authAnswer = coap(sent.get(0));
        // Replies go on the device's next channel sample, not behind a 1 s train.
        assertEquals(TxRequest.TRAIN_ANCHORED, sent.get(0).flags());
        assertEquals(CoapMessage.Type.ACK, authAnswer.type());
        assertEquals(0x0101, authAnswer.mid());
        assertEquals(0x025e, ((authAnswer.payload()[0] & 0xff) << 8) | (authAnswer.payload()[1] & 0xff));
        assertArrayEquals(JoinSession.sessionId(joiner), Arrays.copyOfRange(authAnswer.payload(), 3, 11));

        CoapMessage time = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 1), 0x0102,
                new byte[] {2}, List.of(CoapMessage.path("time")), new byte[0]);
        session.onEvent(fromJoiner(false, time));

        CoapMessage timeAnswer = coap(sent.get(1));
        long seconds = (timeAnswer.payload()[1] & 0xffL) | (timeAnswer.payload()[2] & 0xffL) << 8
                | (timeAnswer.payload()[3] & 0xffL) << 16 | (timeAnswer.payload()[4] & 0xffL) << 24;
        assertEquals(now.getEpochSecond(), seconds);
    }

    @Test
    void servingSendsAnUnsolicitedRouterAdvertisementEvery200To600Seconds() throws Exception {
        JoinSession serving = JoinSession.serving(coordinator, networkKey, radio, clock,
                new SplittableRandom(1), msg -> { });
        now = now.plus(JoinSession.RA_INTERVAL_MIN).minusSeconds(1);
        serving.tick();
        assertTrue(sent.isEmpty());

        now = now.plus(JoinSession.RA_INTERVAL_MAX.minus(JoinSession.RA_INTERVAL_MIN)).plusSeconds(1);
        serving.tick();
        // One copy under the network key: with no pair window there is no pairing-key copy.
        assertEquals(1, sent.size());
        assertEquals(TxRequest.TRAIN_FULL, sent.get(0).flags());
        assertTrue(sent.get(0).dst().isEmpty());
        assertEquals(134, Datagram.parse(sent.get(0).packet(), coordinator, Optional.empty()).payload()[0] & 0xff);

        serving.onEvent(new Event.TxDone(1, Event.TxDone.Result.SENT));
        now = now.plus(JoinSession.RA_INTERVAL_MIN).minusSeconds(1);
        serving.tick();
        assertEquals(1, sent.size());
        now = now.plus(JoinSession.RA_INTERVAL_MAX.minus(JoinSession.RA_INTERVAL_MIN)).plusSeconds(1);
        serving.tick();
        assertEquals(2, sent.size());
    }

    @Test
    void servingAnswersASolicitationUnderTheNetworkKeyOnly() throws Exception {
        JoinSession serving = JoinSession.serving(coordinator, networkKey, radio, clock,
                new SplittableRandom(1), msg -> { });
        byte[] rs = {0x7b, 0x3b, 58, 0x02, (byte) 133, 0, 0, 0, 0, 0, 0, 0};
        serving.onEvent(new Event.Rx(false, true, false, -50, 1, joiner, 1, 0, rs));

        assertEquals(1, sent.size());
        assertEquals(TxRequest.TRAIN_FULL, sent.get(0).flags());
    }

    @Test
    void answersARepeatAgainWhenTheTxDoneOfTheFirstAnswerWasLost() throws Exception {
        CoapMessage put = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 3), 0xa340, new byte[0],
                List.of(CoapMessage.path("z"), CoapMessage.path("p")), new byte[] {0x40, 0x60});
        session.onEvent(fromJoiner(false, put)); // its TX_DONE never comes
        now = now.plus(JoinSession.TX_DONE_TIMEOUT);
        session.onEvent(fromJoiner(false, put));

        assertEquals(2, sent.size());
        assertArrayEquals(sent.get(0).packet(), sent.get(1).packet());
    }

    @Test
    void answersARepeatedRequestOnceWhileItsAnswerIsOnItsWay() throws Exception {
        CoapMessage put = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 3), 0xa339, new byte[0],
                List.of(CoapMessage.path("z"), CoapMessage.path("p")), new byte[] {0x40, 0x60});
        session.onEvent(fromJoiner(false, put));
        session.onEvent(fromJoiner(false, put)); // the device repeats before our answer lands
        assertEquals(1, sent.size());

        session.onEvent(new Event.TxDone(1, Event.TxDone.Result.NO_ACK));
        session.onEvent(fromJoiner(false, put)); // it repeats again: the same answer goes again
        assertEquals(2, sent.size());
        assertArrayEquals(sent.get(0).packet(), sent.get(1).packet());

        CoapMessage next = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 3), 0xa33a, new byte[0],
                List.of(CoapMessage.path("z"), CoapMessage.path("p")), new byte[] {0x40, 0x60});
        session.onEvent(fromJoiner(false, next)); // a new MID is a new request
        assertEquals(3, sent.size());
        assertEquals(0xa33a, coap(sent.get(2)).mid());
    }

    @Test
    void servesTheInstallationsDevicesTheirConfigAndZoneDocument() throws Exception {
        JoinSession serving = JoinSession.serving(coordinator, networkKey, radio, clock, new SplittableRandom(1),
                msg -> { }).installation(new Installation(4242, java.util.Map.of(11, joiner), java.util.Map.of()));
        serving.onEvent(fromJoiner(false, get(0x0201, "d", "VA0000000001", "config")));
        CoapMessage config = coap(sent.get(0));
        assertEquals(CoapMessage.code(2, 5), config.code());
        assertArrayEquals(tadocoord.tado.ConfigDocuments.deviceConfig(4242,
                List.of(new tadocoord.tado.ConfigDocuments.ZoneRole(0x0d, 11))), config.payload());

        serving.onEvent(fromJoiner(false, get(0x0202, "h", "4242", "z", "11", "config")));
        CoapMessage zone = coap(sent.get(1));
        assertEquals(CoapMessage.code(2, 5), zone.code());
        assertEquals(64, zone.payload().length); // block 0 of a longer document

        serving.onEvent(fromJoiner(false, get(0x0205, "h", "4242", "z", "11", "s")));
        CoapMessage state = coap(sent.get(2));
        assertArrayEquals(tadocoord.tado.ConfigDocuments.zoneState(Installation.DEFAULT_SETPOINT), state.payload());

        serving.onEvent(fromJoiner(false, get(0x0203, "h", "4242", "z", "12", "config")));
        serving.onEvent(fromJoiner(false, get(0x0204, "h", "1", "z", "11", "config")));
        assertEquals(3, sent.size()); // no zone 12, no home 1: not ours to answer
    }

    @Test
    void keepsASetpointSetOnTheDeviceForTheOverlayTimeThenWritesTheZoneBack() throws Exception {
        JoinSession serving = servingZone11(java.time.Duration.ofHours(1));
        serving.onEvent(fromJoiner(false, overlayPut(0x0301, "62800208346240010262600101"))); // 21.00 C, manual
        assertEquals(CoapMessage.code(2, 4), coap(unicasts().get(0)).code());

        now = now.plusSeconds(59 * 60);
        serving.onEvent(fromJoiner(false, get(0x0302, "h", "4242", "z", "11", "s")));
        assertArrayEquals(tadocoord.tado.ConfigDocuments.zoneState(1800, java.util.OptionalInt.of(2100)),
                coap(unicasts().get(1)).payload());

        now = now.plusSeconds(60);
        serving.tick();
        assertEquals(3, unicasts().size());
        TxRequest write = unicasts().get(2);
        assertEquals(TxRequest.TRAIN_ANCHORED, write.flags());
        CoapMessage w = coap(write);
        assertEquals(CoapMessage.Type.CON, w.type());
        assertEquals(CoapMessage.code(0, 3), w.code());
        assertEquals("z/s", w.path());
        assertArrayEquals("id=11".getBytes(), option(w, 15));
        assertArrayEquals(tadocoord.tado.ConfigDocuments.zoneState(1800), w.payload()); // 6240=0, no 6440

        serving.onEvent(fromJoiner(false, get(0x0303, "h", "4242", "z", "11", "s")));
        assertArrayEquals(tadocoord.tado.ConfigDocuments.zoneState(1800), coap(unicasts().get(3)).payload());

        serving.onEvent(fromJoiner(false, ack(w.mid(), CoapMessage.code(2, 4))));
        now = now.plusSeconds(60);
        serving.tick();
        assertEquals(4, unicasts().size()); // acknowledged: not again
    }

    @Test
    void repeatsAnUnacknowledgedZoneWriteUnderItsMidThenGivesUp() throws Exception {
        JoinSession serving = servingZone11(java.time.Duration.ofMinutes(1));
        serving.onEvent(fromJoiner(false, overlayPut(0x0311, "62800208346240010262600101")));
        now = now.plusSeconds(60);
        serving.tick();
        int mid = coap(unicasts().get(1)).mid();
        for (int i = 0; i < 10; i++) {
            now = now.plus(JoinSession.ZONE_WRITE_TIMEOUT);
            serving.tick();
        }
        assertEquals(1 + JoinSession.ZONE_WRITE_ATTEMPTS, unicasts().size());
        for (int i = 1; i < unicasts().size(); i++) {
            assertEquals(mid, coap(unicasts().get(i)).mid());
        }
    }

    @Test
    void anOverlayTheDeviceEndsOrThatIsNotItsZoneLeavesNothingToWrite() throws Exception {
        JoinSession serving = servingZone11(java.time.Duration.ofMinutes(1));
        serving.onEvent(fromJoiner(false, overlayPut(0x0321, "62800208346240010262600101")));
        serving.onEvent(fromJoiner(false, overlayPut(0x0322, "6240010062600101"))); // ended on the device
        CoapMessage other = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 3), 0x0323, new byte[0],
                List.of(CoapMessage.path("h"), CoapMessage.path("4242"), CoapMessage.path("z"),
                        CoapMessage.path("12"), CoapMessage.path("overlay")),
                java.util.HexFormat.of().parseHex("62800208346240010262600101"));
        serving.onEvent(fromJoiner(false, other)); // zone 12 is not the joiner's
        now = now.plusSeconds(120);
        serving.tick();
        assertEquals(3, unicasts().size()); // three 2.04s, no write

        serving.onEvent(fromJoiner(false, get(0x0324, "h", "4242", "z", "11", "s")));
        assertArrayEquals(tadocoord.tado.ConfigDocuments.zoneState(1800), coap(unicasts().get(3)).payload());
    }

    @Test
    void anOverlayTimeOfZeroKeepsTheDevicesSetpointUntilChanged() throws Exception {
        JoinSession serving = servingZone11(java.time.Duration.ZERO);
        serving.onEvent(fromJoiner(false, overlayPut(0x0331, "62800208346240010262600101")));
        now = now.plus(java.time.Duration.ofDays(30));
        serving.tick();
        serving.onEvent(fromJoiner(false, get(0x0332, "h", "4242", "z", "11", "s")));
        assertEquals(2, unicasts().size());
        assertArrayEquals(tadocoord.tado.ConfigDocuments.zoneState(1800, java.util.OptionalInt.of(2100)),
                coap(unicasts().get(1)).payload());
    }

    private JoinSession servingZone11(java.time.Duration deviceOverlay) {
        return JoinSession.serving(coordinator, networkKey, radio, clock, new SplittableRandom(1), msg -> { })
                .installation(new Installation(4242, java.util.Map.of(11, joiner), java.util.Map.of(), deviceOverlay));
    }

    /** The device's report of a setpoint set on it, as the sensor sent it. */
    private static CoapMessage overlayPut(int mid, String body) {
        return new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 3), mid, new byte[0],
                List.of(CoapMessage.path("h"), CoapMessage.path("4242"), CoapMessage.path("z"),
                        CoapMessage.path("11"), CoapMessage.path("overlay")),
                java.util.HexFormat.of().parseHex(body));
    }

    /** What went to the device: not the periodic Router Advertisements a long wait brings. */
    private List<TxRequest> unicasts() {
        return sent.stream().filter(r -> r.dst().isPresent()).toList();
    }

    private static byte[] option(CoapMessage m, int number) {
        return m.options().stream().filter(o -> o.number() == number).findFirst().orElseThrow().value();
    }

    private static CoapMessage get(int mid, String... path) {
        List<CoapMessage.Option> options = new java.util.ArrayList<>();
        for (String p : path) {
            options.add(CoapMessage.path(p));
        }
        options.add(new CoapMessage.Option(23, new byte[] {0x03})); // Block2 0/0/128
        return new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 1), mid, new byte[0], options, new byte[0]);
    }

    @Test
    void answersAFragmentedReportWith204() throws Exception {
        CoapMessage put = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 3), 0xa302, new byte[] {7},
                List.of(CoapMessage.path("d"), CoapMessage.path("SU0000000001"), CoapMessage.path("fw"),
                        CoapMessage.path("state")), new byte[40]);
        byte[] packet = Datagram.udp(joiner, coordinator, 5683, 5683, put.encode());
        // RFC 4944: FRAG1 holds the 9 compressed header bytes (48 uncompressed) and 16 CoAP bytes,
        // so the FRAGN starts at offset 64 (8 x 8).
        int size = 48 + packet.length - 9;
        byte[] frag1 = concat(new byte[] {(byte) (0xc0 | size >>> 8), (byte) size, 0, 5},
                Arrays.copyOfRange(packet, 0, 9 + 16));
        byte[] fragN = concat(new byte[] {(byte) (0xe0 | size >>> 8), (byte) size, 0, 5, 8},
                Arrays.copyOfRange(packet, 9 + 16, packet.length));

        session.onEvent(new Event.Rx(false, false, true, -50, 2, joiner, 2, 0, frag1));
        assertTrue(sent.isEmpty());
        session.onEvent(new Event.Rx(false, false, true, -50, 3, joiner, 3, 0, fragN));

        CoapMessage answer = coap(sent.get(0));
        assertEquals(CoapMessage.Type.ACK, answer.type());
        assertEquals(CoapMessage.code(2, 4), answer.code());
        assertEquals(0xa302, answer.mid());
        assertArrayEquals(new byte[] {7}, answer.token());
    }

    @Test
    void ignoresRequestsUnderThePairingKey() throws Exception {
        CoapMessage time = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 1), 0x0102,
                new byte[0], List.of(CoapMessage.path("time")), new byte[0]);
        session.onEvent(fromJoiner(true, time));
        assertTrue(sent.isEmpty());
        assertNotEquals(JoinSession.State.FAILED, session.state());
    }

    private Event.Rx routerSolicitation() {
        // IPHC: TF elided, NH inline (ICMPv6), hop limit 255, src from L2, dst ff02::2. Checksum not checked.
        byte[] packet = {0x7b, 0x3b, 58, 0x02, (byte) 133, 0, 0, 0, 0, 0, 0, 0};
        return new Event.Rx(true, true, false, -50, 1, joiner, 1, 0, packet);
    }

    private Event.Rx fromJoiner(boolean pairingKey, CoapMessage m) {
        byte[] packet = Datagram.udp(joiner, coordinator, 5683, 5683, m.encode());
        return new Event.Rx(pairingKey, false, true, -50, 2, joiner, 2, 0, packet);
    }

    private static byte[] concat(byte[] a, byte[] b) {
        byte[] out = Arrays.copyOf(a, a.length + b.length);
        System.arraycopy(b, 0, out, a.length, b.length);
        return out;
    }

    private static CoapMessage ack(int mid, int code) {
        return new CoapMessage(CoapMessage.Type.ACK, code, mid, new byte[0], List.of(), new byte[0]);
    }

    private CoapMessage coap(TxRequest r) {
        Datagram d = Datagram.parse(r.packet(), coordinator, r.dst());
        return CoapMessage.decode(d.payload());
    }
}
