package tadocoord.join;

import java.io.IOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.time.Instant;
import java.time.InstantSource;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalInt;
import java.util.Set;
import java.util.function.Consumer;
import java.util.random.RandomGenerator;
import tadocoord.Eui;
import tadocoord.coap.CoapMessage;
import tadocoord.link.Event;
import tadocoord.link.Radio;
import tadocoord.link.Radio.TxRequest;
import tadocoord.lowpan.Datagram;
import tadocoord.lowpan.Ipv6;
import tadocoord.lowpan.Reassembler;
import tadocoord.tado.ConfigDocuments;
import tadocoord.tado.TadoResources;

/**
 * Drives one device through the join exchange of docs/JOIN.md: Router Advertisements for its
 * solicitations, {@code d/pair} with our network key, then {@code auth/token} and {@code time}.
 *
 * <p>Not thread-safe: feed events and ticks from one thread.
 */
public final class JoinSession {
    /** RFC 4861 MIN_DELAY_BETWEEN_RAS, which the commercial bridge follows. */
    static final Duration RA_MIN_GAP = Duration.ofSeconds(3);
    /** How long to wait for the 2.04 after {@code d/pair} left the radio before trying again. */
    static final Duration PAIR_ANSWER_TIMEOUT = Duration.ofSeconds(6);
    static final int PAIR_ATTEMPTS = 4;
    /**
     * HOSTLINK v0 may lose a TX_DONE. A frame still unreported after this long (two 1 s trains
     * plus queueing) is taken as having left the radio, so the session never stalls on it.
     */
    static final Duration TX_DONE_TIMEOUT = Duration.ofSeconds(5);
    /**
     * Unsolicited Router Advertisements go out every 200-600 s, as the bridge's (RFC 4861
     * MaxRtrAdvInterval 600 s, MinRtrAdvInterval a third of it). A device that hears none for
     * about 20 minutes stops reporting and only solicits.
     */
    static final Duration RA_INTERVAL_MIN = Duration.ofSeconds(200);
    static final Duration RA_INTERVAL_MAX = Duration.ofSeconds(600);
    /**
     * How long an answer is kept for a repeated request (same device and MID). A device
     * retries a CON request 2-3 s apart, up to three times.
     */
    static final Duration ANSWER_LIFETIME = Duration.ofSeconds(60);

    private static final int ICMP_ROUTER_SOLICITATION = 133;
    private static final int PUT = CoapMessage.code(0, 3);
    private static final int POST = CoapMessage.code(0, 2);
    private static final int GET = CoapMessage.code(0, 1);
    private static final byte[] NO_IPV4 = new byte[4];

    /** SERVING: no pair window; serves the devices already on the network. */
    public enum State { WAITING_FOR_JOINER, PAIRING, JOINED, FAILED, SERVING }

    private final Eui coordinator;
    private final byte[] networkKey;
    private final Radio radio;
    private final InstantSource clock;
    private final RandomGenerator rng;
    private final Consumer<String> progress;
    private final Reassembler reassembler = new Reassembler();
    private Installation installation = Installation.NONE;
    /** Our answer to each recent request, for its repeats. */
    private final Map<Exchange, Answer> answers = new HashMap<>();

    private record Exchange(Eui device, int mid) {}

    private static final class Answer {
        final byte[] packet;
        final Instant at;
        int handle; // -1 once the radio is done with it

        Answer(byte[] packet, Instant at) {
            this.packet = packet;
            this.at = at;
        }
    }

    private State state = State.WAITING_FOR_JOINER;
    private Optional<Eui> joiner = Optional.empty();
    private int nextMid;

    private int raHandle = -1; // last RA copy in flight
    private Instant raSent = Instant.MIN;
    private Instant lastRaEnded = Instant.MIN;
    private Instant nextRa;

    private int pairHandle = -1;
    private Instant pairSent = Instant.MIN;
    /** Every attempt's MID: the valve may answer an earlier attempt after we sent the next. */
    private final Set<Integer> pairMids = new HashSet<>();
    private int pairAttempts;
    private Optional<Instant> pairDeadline = Optional.empty();

    public JoinSession(Eui coordinator, byte[] networkKey, Radio radio, InstantSource clock,
            RandomGenerator rng, Consumer<String> progress) {
        this.coordinator = coordinator;
        this.networkKey = networkKey.clone();
        this.radio = radio;
        this.clock = clock;
        this.rng = rng;
        this.progress = progress;
        this.nextMid = rng.nextInt(0x10000);
        scheduleNextRa();
    }

    /** Serves the devices of {@code i} their configuration and zone documents. */
    public JoinSession installation(Installation i) {
        this.installation = i;
        return this;
    }

    /** A session with no pair window: it only keeps the joined devices served. */
    public static JoinSession serving(Eui coordinator, byte[] networkKey, Radio radio, InstantSource clock,
            RandomGenerator rng, Consumer<String> progress) {
        JoinSession s = new JoinSession(coordinator, networkKey, radio, clock, rng, progress);
        s.state = State.SERVING;
        return s;
    }

    public State state() {
        return state;
    }

    public void onEvent(Event event) throws IOException {
        switch (event) {
            case Event.BeaconAnswered b -> onBeaconAnswered(b);
            case Event.Rx rx -> onRx(rx);
            case Event.TxDone done -> onTxDone(done);
            case Event.Log log -> progress.accept("cc1312: " + log.text());
            default -> { }
        }
    }

    /** Call periodically (every few hundred ms) to drive retries. */
    public void tick() throws IOException {
        Instant now = clock.instant();
        if (raHandle >= 0 && !now.isBefore(raSent.plus(TX_DONE_TIMEOUT))) {
            progress.accept("no TX_DONE for the router advertisement, assuming it went out");
            raEnded();
        }
        if (pairHandle >= 0 && !now.isBefore(pairSent.plus(TX_DONE_TIMEOUT))) {
            progress.accept("no TX_DONE for d/pair, assuming it went out");
            pairLeftRadio();
        }
        answers.values().removeIf(a -> !now.isBefore(a.at.plus(ANSWER_LIFETIME)));
        if (raHandle < 0 && !now.isBefore(nextRa) && raAllowed()) {
            sendRouterAdvert();
        }
        if (state == State.PAIRING && pairDeadline.isPresent() && !clock.instant().isBefore(pairDeadline.get())) {
            pairDeadline = Optional.empty();
            if (pairAttempts >= PAIR_ATTEMPTS) {
                state = State.FAILED;
                progress.accept("no 2.04 to d/pair after " + PAIR_ATTEMPTS + " attempts");
            } else {
                progress.accept("no 2.04 to d/pair yet, sending it again");
                sendPair();
            }
        }
    }

    private void onBeaconAnswered(Event.BeaconAnswered b) {
        progress.accept("beacon answered for " + b.joiner() + " (rssi " + b.rssi() + ")");
        if (joiner.isEmpty()) {
            joiner = Optional.of(b.joiner());
        }
    }

    private void onRx(Event.Rx rx) throws IOException {
        Datagram d;
        try {
            Optional<byte[]> whole = reassembler.add(rx.src(), rx.packet(), clock.instant());
            if (whole.isEmpty()) {
                return; // a fragment of a datagram still incomplete
            }
            d = Datagram.parse(whole.get(), rx.src(), rx.broadcast() ? Optional.empty() : Optional.of(coordinator));
        } catch (IllegalArgumentException e) {
            progress.accept("unparsed datagram from " + rx.src() + ": " + e.getMessage());
            return;
        }
        if (d.icmpType().equals(OptionalInt.of(ICMP_ROUTER_SOLICITATION))) {
            onRouterSolicitation(rx);
        } else if (d.nextHeader() == Ipv6.NH_UDP && !rx.broadcast()) {
            onCoap(rx, d);
        }
    }

    private void onRouterSolicitation(Event.Rx rx) throws IOException {
        progress.accept("router solicitation from " + rx.src() + (rx.pairingKey() ? " (pairing key)" : ""));
        if (rx.pairingKey() && joiner.isEmpty()) {
            joiner = Optional.of(rx.src());
        }
        // One in flight answers this solicitation too; within 3 s of the last, RFC 4861 sends none.
        if (raHandle < 0 && raAllowed()) {
            sendRouterAdvert();
        } else if (raHandle < 0 && state == State.WAITING_FOR_JOINER && joiner.isPresent()) {
            sendPair(); // the advertisement that just ended reached the joiner too
        }
    }

    private void onCoap(Event.Rx rx, Datagram d) throws IOException {
        CoapMessage m;
        try {
            m = CoapMessage.decode(d.payload());
        } catch (IllegalArgumentException e) {
            progress.accept("bad CoAP from " + rx.src() + ": " + e.getMessage());
            return;
        }
        boolean fromJoiner = joiner.isPresent() && joiner.get().equals(rx.src());
        if (fromJoiner && state == State.PAIRING && isPairAnswer(m)) {
            if (m.type() == CoapMessage.Type.ACK && m.code() == 0) {
                // Empty ACK: the 2.04 follows as a separate response (RFC 7252 5.2.2).
                progress.accept("d/pair acknowledged, waiting for the separate answer");
                pairDeadline = Optional.of(clock.instant().plus(PAIR_ANSWER_TIMEOUT));
                return;
            }
            if (m.type() == CoapMessage.Type.CON) {
                // Answer under the key it came in, so the ACK reaches it either way.
                int key = rx.pairingKey() ? TxRequest.PAIRING_KEY : 0;
                send(TxRequest.unicast(rx.src(), TxRequest.TRAIN_FULL | key,
                        TadoResources.emptyAck(m, d, coordinator, rx.src())));
            }
            onPairAnswer(rx, m);
            return;
        }
        if (rx.pairingKey()) {
            return; // only d/pair's answer matters before the device holds our key
        }
        String path = m.path();
        Exchange exchange = new Exchange(rx.src(), m.mid());
        Answer earlier = m.type() == CoapMessage.Type.CON ? answers.get(exchange) : null;
        if (earlier != null) {
            // A repeat: the same answer again, unless it is still on its way.
            if (earlier.handle < 0) {
                progress.accept("%s from %s again, answering it again".formatted(path, rx.src()));
                earlier.handle = send(TxRequest.unicast(rx.src(), TxRequest.TRAIN_ANCHORED, earlier.packet));
            }
            return;
        }
        Optional<byte[]> document = m.type() == CoapMessage.Type.CON && m.code() == GET
                ? documentFor(rx.src(), path) : Optional.empty();
        if (document.isPresent()) {
            progress.accept("GET %s from %s, block %s".formatted(path, rx.src(), blockOf(m)));
            answer(exchange, ConfigDocuments.blockAnswer(m, document.get(), d, coordinator, rx.src()));
        } else if (m.type() == CoapMessage.Type.CON && (path.equals("auth/token") || path.equals("auth/key"))) {
            progress.accept(path + " from " + rx.src() + ", answering with a session");
            answer(exchange, TadoResources.authTokenAnswer(m, sessionId(rx.src()), d, coordinator, rx.src()));
        } else if (m.type() == CoapMessage.Type.CON && path.equals("time")) {
            progress.accept("time from " + rx.src());
            answer(exchange, TadoResources.timeAnswer(m, clock.instant().getEpochSecond(), d, coordinator, rx.src()));
        } else if (m.type() == CoapMessage.Type.CON && (m.code() == PUT || m.code() == POST)) {
            // Reports (d/<serial>/..., z/p): the bridge's default 2.04, or the device retries.
            progress.accept("%s %s from %s, answering 2.04: %s".formatted(CoapMessage.codeString(m.code()), path,
                    rx.src(), HexFormat.of().formatHex(m.payload())));
            answer(exchange, TadoResources.changedAnswer(m, d, coordinator, rx.src()));
        } else {
            progress.accept("CoAP %s %s %s from %s (not handled yet)".formatted(
                    m.type(), CoapMessage.codeString(m.code()), path, rx.src()));
        }
    }

    /**
     * A piggybacked answer (ACK with one of our d/pair MIDs), or a separate response: a CON/NON
     * response code with d/pair's empty token.
     */
    private boolean isPairAnswer(CoapMessage m) {
        if (m.type() == CoapMessage.Type.ACK) {
            return pairMids.contains(m.mid());
        }
        boolean response = (m.code() >>> 5) >= 2;
        return response && m.token().length == 0
                && (m.type() == CoapMessage.Type.CON || m.type() == CoapMessage.Type.NON);
    }

    private void onPairAnswer(Event.Rx rx, CoapMessage m) {
        pairDeadline = Optional.empty();
        if (rx.pairingKey()) {
            progress.accept("d/pair answered under the pairing key: device did not adopt our key");
            state = State.FAILED;
        } else if (m.code() == CoapMessage.code(2, 4)) {
            progress.accept("2.04 under our network key: " + rx.src() + " JOINED");
            state = State.JOINED;
        } else {
            progress.accept("d/pair answered " + CoapMessage.codeString(m.code()));
            state = State.FAILED;
        }
    }

    /**
     * The configuration ({@code d/<serial>/config}), zone document ({@code h/<home>/z/<n>/config})
     * or zone state ({@code h/<home>/z/<n>/s}) for a device of the installation; empty for
     * anything else.
     */
    private Optional<byte[]> documentFor(Eui device, String path) {
        String[] s = path.split("/");
        if (s.length == 3 && s[0].equals("d") && s[2].equals("config")) {
            List<ConfigDocuments.ZoneRole> roles = new ArrayList<>();
            installation.zoneOwners().forEach((zone, owner) -> {
                if (owner.equals(device)) {
                    roles.add(new ConfigDocuments.ZoneRole(ConfigDocuments.ROLE_OWNER, zone));
                }
            });
            return roles.isEmpty() ? Optional.empty()
                    : Optional.of(ConfigDocuments.deviceConfig(installation.homeId(), roles));
        }
        if (s.length == 5 && s[0].equals("h") && s[1].equals(Long.toString(installation.homeId()))
                && s[2].equals("z") && s[3].matches("\\d+")) {
            int zone = Integer.parseInt(s[3]);
            Eui owner = installation.zoneOwners().get(zone);
            if (owner == null) {
                return Optional.empty();
            }
            return switch (s[4]) {
                case "config" -> Optional.of(ConfigDocuments.zoneWithOwner(owner));
                case "s" -> Optional.of(ConfigDocuments.zoneState(installation.setpoint(zone)));
                default -> Optional.empty();
            };
        }
        return Optional.empty();
    }

    private static String blockOf(CoapMessage m) {
        return m.options().stream().filter(o -> o.number() == 23).findFirst()
                .map(o -> HexFormat.of().formatHex(o.value())).orElse("-");
    }

    /** Sends an answer on the device's sample and keeps it for the request's repeats. */
    private void answer(Exchange exchange, byte[] packet) throws IOException {
        Answer a = new Answer(packet, clock.instant());
        a.handle = send(TxRequest.unicast(exchange.device(), TxRequest.TRAIN_ANCHORED, packet));
        answers.put(exchange, a);
    }

    private void onTxDone(Event.TxDone done) throws IOException {
        for (Answer a : answers.values()) {
            if (a.handle == done.handle()) {
                a.handle = -1;
            }
        }
        if (done.handle() == raHandle) {
            raEnded();
        } else if (done.handle() == pairHandle) {
            progress.accept("d/pair " + done.result());
            pairLeftRadio();
        }
    }

    private void raEnded() throws IOException {
        raHandle = -1;
        lastRaEnded = clock.instant();
        if (state == State.WAITING_FOR_JOINER && joiner.isPresent()) {
            sendPair(); // as the bridge does: d/pair follows the advertisement for the joiner
        }
    }

    private void pairLeftRadio() {
        pairHandle = -1;
        pairDeadline = Optional.of(clock.instant().plus(PAIR_ANSWER_TIMEOUT));
    }

    /** During the pair window every RA goes twice: network key with Frame Pending, then pairing key. */
    private void sendRouterAdvert() throws IOException {
        byte[] ra = TadoResources.routerAdvert(coordinator, NO_IPV4, NO_IPV4);
        scheduleNextRa();
        if (state == State.JOINED || state == State.SERVING) {
            raHandle = send(TxRequest.broadcast(TxRequest.TRAIN_FULL, ra));
            raSent = clock.instant();
            progress.accept("router advertisement sent");
            return;
        }
        send(TxRequest.broadcast(TxRequest.FRAME_PENDING | TxRequest.TRAIN_FULL, ra));
        raHandle = send(TxRequest.broadcast(TxRequest.PAIRING_KEY | TxRequest.TRAIN_FULL, ra));
        raSent = clock.instant();
        progress.accept("router advertisement sent (network + pairing key)");
    }

    private void sendPair() throws IOException {
        Eui j = joiner.orElseThrow();
        byte[] session = new byte[8];
        rng.nextBytes(session);
        int mid = nextMid++ & 0xffff;
        pairMids.add(mid);
        pairAttempts++;
        state = State.PAIRING;
        pairHandle = send(TxRequest.unicast(j, TxRequest.PAIRING_KEY | TxRequest.TRAIN_FULL,
                TadoResources.pairRequest(networkKey, mid, session, coordinator, j)));
        pairSent = clock.instant();
        progress.accept("d/pair to " + j + " (attempt " + pairAttempts + ")");
    }

    private int send(TxRequest r) throws IOException {
        return radio.tx(r).handle();
    }

    private void scheduleNextRa() {
        long spread = RA_INTERVAL_MAX.minus(RA_INTERVAL_MIN).toMillis();
        nextRa = clock.instant().plus(RA_INTERVAL_MIN).plusMillis(rng.nextLong(spread + 1));
    }

    private boolean raAllowed() {
        return !clock.instant().isBefore(lastRaEnded.plus(RA_MIN_GAP));
    }

    /** Stable per device, so a restart serves the same id (as freedo does). */
    static byte[] sessionId(Eui device) {
        try {
            return Arrays.copyOf(MessageDigest.getInstance("SHA-256").digest(device.wire()), 8);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }
}
