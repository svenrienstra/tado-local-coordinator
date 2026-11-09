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

    private static CoapMessage ack(int mid, int code) {
        return new CoapMessage(CoapMessage.Type.ACK, code, mid, new byte[0], List.of(), new byte[0]);
    }

    private CoapMessage coap(TxRequest r) {
        Datagram d = Datagram.parse(r.packet(), coordinator, r.dst());
        return CoapMessage.decode(d.payload());
    }
}
