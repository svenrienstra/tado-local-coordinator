package tadocoord.bench;

import java.nio.file.Path;
import java.time.Instant;
import java.util.HexFormat;
import java.util.Optional;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import tadocoord.Eui;
import tadocoord.Main;
import tadocoord.coap.CoapMessage;
import tadocoord.link.Event;
import tadocoord.link.HostLink;
import tadocoord.link.Radio.TxRequest;
import tadocoord.link.SerialTransport;
import tadocoord.lowpan.Datagram;
import tadocoord.lowpan.Ipv6;
import tadocoord.lowpan.Reassembler;

/**
 * The join probe (docs/PROBE.md): pose as a joining device to a tado coordinator (your own
 * bridge, on an isolated network) and read what it sends in {@code d/pair}. Receive-only — it
 * adopts no key, sends no {@code 2.04}, runs no {@code auth/token}.
 *
 * <p>No probe firmware needed: the coordinator firmware already on the CC1312 does the work.
 * CONFIG sets a chosen joiner EUI; {@code HL_TX} with the pairing-key flag broadcasts a Router
 * Solicitation sealed under the pairing key from that EUI; and with a pair window open the
 * firmware opens the bridge's {@code d/pair} (unicast to us under the pairing key) and hands it
 * back decoded, so we only read the key TLV tag: {@code 0x12} plaintext or {@code 0x07} wrapped.
 *
 * <p>This only sends a Router Solicitation. If the bridge first wants the Enhanced Beacon Request
 * that a real device broadcasts (which this firmware cannot send), it will not reach {@code d/pair}
 * here; that is the signal to flash the probe firmware built around {@code probe/joiner.c}.
 *
 * <p>{@code JoinProbe <port> [--eui <hex>] [--channel <kHz>] [--seconds <n>] [--period <s>] [--beacon]}
 */
public final class JoinProbe {
    private static final int ICMP_ROUTER_SOLICITATION = 133;
    private static final int ALL_ROUTERS = 2; // ff02::2
    private static final Eui DEFAULT_JOINER = Eui.ofHex("a1b2c3d407c51b00"); // novel, tado suffix

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: JoinProbe <port> [--eui <hex>] [--channel <kHz>] "
                    + "[--seconds <n>] [--period <s>] [--beacon]");
            System.exit(2);
        }
        Eui joiner = DEFAULT_JOINER;
        Integer channelKhz = null;
        int seconds = 180;
        int periodSeconds = 3;
        boolean beacon = false; // --beacon: send an Enhanced Beacon Request first (PROBE firmware)
        for (int i = 1; i < args.length; i++) {
            switch (args[i]) {
                case "--eui" -> joiner = Eui.ofHex(args[++i]);
                case "--channel" -> channelKhz = Integer.parseInt(args[++i]);
                case "--seconds" -> seconds = Integer.parseInt(args[++i]);
                case "--period" -> periodSeconds = Integer.parseInt(args[++i]);
                case "--beacon" -> beacon = true;
                default -> throw new IllegalArgumentException("unknown arg " + args[i]);
            }
        }
        if (!joiner.hasTadoSuffix()) {
            System.err.println("warning: " + joiner + " does not end in c5 1b 00; a tado bridge may ignore it");
        }

        // The identity only supplies a channel default and a throwaway network key; we never
        // use the key, since we only send under the pairing key and never adopt one.
        Main.Identity id = Main.Identity.loadOrCreate(Path.of("state/coordinator.properties"));
        int channel = channelKhz != null ? channelKhz : id.channelKhz();

        // A minimal Router Solicitation: type, code, checksum(2, filled), reserved(4).
        byte[] rsMessage = {(byte) ICMP_ROUTER_SOLICITATION, 0, 0, 0, 0, 0, 0, 0};
        byte[] rs = Datagram.icmpv6ToLinkMulticast(joiner, ALL_ROUTERS, rsMessage);

        LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();
        Reassembler reassembler = new Reassembler();
        HexFormat hex = HexFormat.of();
        long t0 = System.nanoTime();
        int unverified = 0;
        boolean pairSeen = false;

        try (HostLink link = new HostLink(new SerialTransport(args[0]), events::add)) {
            link.ping();
            link.configure(joiner, id.networkKey(), id.counterFloor(), channel);
            link.pairWindow(seconds + 30); // lets the firmware open the bridge's pairing-key d/pair
            System.out.printf("posing as %s on %d kHz for %d s%n", joiner, channel, seconds);
            System.out.println(">>> put the bridge in pairing mode (press its pair button) now <<<");
            System.out.printf("sending a Router Solicitation every %d s; waiting for d/pair%n%n", periodSeconds);

            long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(seconds);
            long nextRs = 0;
            int rsCount = 0;
            while (System.nanoTime() < end && !pairSeen) {
                if (System.nanoTime() >= nextRs) {
                    if (beacon) {
                        link.sendBeacon();
                        System.out.printf("%6.3f s  -> Enhanced Beacon Request%n", elapsed(t0));
                    }
                    link.tx(TxRequest.broadcast(TxRequest.PAIRING_KEY, rs));
                    System.out.printf("%6.3f s  -> Router Solicitation #%d%n", elapsed(t0), ++rsCount);
                    nextRs = System.nanoTime() + TimeUnit.SECONDS.toNanos(periodSeconds);
                }
                Event e = events.poll(200, TimeUnit.MILLISECONDS);
                if (e == null) {
                    continue;
                }
                double t = elapsed(t0);
                switch (e) {
                    case Event.Rx rx when rx.pairingKey() -> {
                        // Opened under the pairing key with our window open: this is the bridge
                        // talking to us. The d/pair we are after arrives here, decoded.
                        Optional<byte[]> whole = reassembler.add(rx.src(), rx.packet(), Instant.now());
                        if (whole.isEmpty()) {
                            continue;
                        }
                        Datagram d = Datagram.parse(whole.get(), rx.src(),
                                rx.broadcast() ? Optional.empty() : Optional.of(joiner));
                        if (d.nextHeader() == Ipv6.NH_UDP) {
                            CoapMessage m = CoapMessage.decode(d.payload());
                            if (m.path().equals("d/pair")) {
                                pairSeen = true;
                                reportPair(t, rx.src(), m.payload(), hex);
                            } else {
                                System.out.printf("%6.3f s  <- CoAP %s %s from %s  payload %s%n", t,
                                        m.type(), m.path(), rx.src(), hex.formatHex(m.payload()));
                            }
                        } else {
                            System.out.printf("%6.3f s  <- ICMPv6 type %s from %s (pairing key)%n", t,
                                    d.icmpType(), rx.src());
                        }
                    }
                    case Event.Rx rx -> System.out.printf("%6.3f s  <- Rx (network key) from %s%n", t, rx.src());
                    case Event.RxUnverified u -> {
                        // The bridge's own traffic (its RA and such) is sealed under its network
                        // key, which we do not hold, so it lands here. Seeing these means it hears us.
                        unverified++;
                        if (unverified <= 20 || unverified % 20 == 0) {
                            System.out.printf("%6.3f s  .. unverified frame #%d, rssi %d, %d bytes%n", t,
                                    unverified, u.rssi(), u.frame().length);
                        }
                    }
                    case Event.BeaconAnswered b ->
                        System.out.printf("%6.3f s  !! bridge answered a beacon from %s (unexpected)%n", t, b.joiner());
                    case Event.Log l -> System.out.printf("%6.3f s  fw: %s%n", t, l.text());
                    case Event.TxDone d -> { /* our RS send result; quiet */ }
                    default -> System.out.printf("%6.3f s  ?? %s%n", t, e);
                }
            }
            System.out.printf("%ndone after %.1f s: %d solicitations, %d unverified frames, d/pair %s%n",
                    elapsed(t0), rsCount, unverified, pairSeen ? "SEEN" : "not seen");
            if (!pairSeen) {
                System.out.println("No d/pair. Likely one of: wrong channel (find it with RxSweep); the "
                        + "bridge wants a beacon first (flash the probe firmware); or an offline bridge "
                        + "will not pair without the cloud.");
            }
        }
    }

    private static void reportPair(double t, Eui src, byte[] tlv, HexFormat hex) {
        int tag = tlv.length > 0 ? tlv[0] & 0xff : -1;
        int len = tlv.length > 1 ? tlv[1] & 0xff : -1;
        System.out.printf("%n%6.3f s  *** d/pair from %s ***%n", t, src);
        switch (tag) {
            case 0x12 -> System.out.printf("    tag 0x12: the network key IN PLAINTEXT, %d bytes. "
                    + "The bridge handed the key in the clear.%n", len);
            case 0x07 -> System.out.printf("    tag 0x07: the key WRAPPED under a per-device KEK, %d bytes. "
                    + "As expected; not openable without the KEK.%n", len);
            default -> System.out.printf("    unexpected TLV tag 0x%02x, %d bytes: %s%n", tag, len,
                    hex.formatHex(tlv));
        }
    }

    private static double elapsed(long t0) {
        return (System.nanoTime() - t0) / 1e9;
    }
}
