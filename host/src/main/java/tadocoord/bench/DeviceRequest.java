package tadocoord.bench;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
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
import tadocoord.lowpan.Reassembler;

/**
 * Sends one CoAP request to a joined device, as the bridge sends commands (from port 4005, a
 * PUT or POST with Uri-Port ffff and Content-Format 42, as freedo's {@code deviceWrite}), behind
 * an anchored train, and prints the device's answer decoded. Other requests from the device
 * are printed, not answered: run it with no {@code serve} on the port.
 *
 * <p>{@code DeviceRequest <port> <device EUI> <GET|PUT|POST> <path, e.g. d/identify or z/s?lid=11>
 * [payload hex] [--wait <s>]}
 */
public final class DeviceRequest {
    private static final int BRIDGE_PORT = 4005;
    private static final int URI_QUERY = 15;

    public static void main(String[] args) throws Exception {
        if (args.length < 4) {
            System.err.println("usage: DeviceRequest <port> <device EUI> <GET|PUT|POST> <path> [payload hex] [--wait <s>]");
            System.exit(2);
        }
        Eui dev = Eui.ofHex(args[1]);
        int code = switch (args[2]) {
            case "GET" -> CoapMessage.code(0, 1);
            case "POST" -> CoapMessage.code(0, 2);
            case "PUT" -> CoapMessage.code(0, 3);
            default -> throw new IllegalArgumentException("method " + args[2]);
        };
        byte[] payload = new byte[0];
        int waitSeconds = 10;
        for (int i = 4; i < args.length; i++) {
            if (args[i].equals("--wait")) {
                waitSeconds = Integer.parseInt(args[++i]);
            } else {
                payload = HexFormat.of().parseHex(args[i]);
            }
        }
        List<CoapMessage.Option> options = new ArrayList<>();
        if (code != CoapMessage.code(0, 1)) {
            options.add(new CoapMessage.Option(7, new byte[] {(byte) 0xff, (byte) 0xff})); // Uri-Port
        }
        String[] pathAndQuery = args[3].split("\\?", 2);
        for (String segment : pathAndQuery[0].split("/")) {
            options.add(CoapMessage.path(segment));
        }
        if (code != CoapMessage.code(0, 1)) {
            options.add(new CoapMessage.Option(CoapMessage.CONTENT_FORMAT, new byte[] {42}));
        }
        if (pathAndQuery.length == 2) {
            for (String q : pathAndQuery[1].split("&")) {
                options.add(new CoapMessage.Option(URI_QUERY, q.getBytes(java.nio.charset.StandardCharsets.US_ASCII)));
            }
        }
        int mid = (int) (System.nanoTime() & 0xffff);
        CoapMessage request = new CoapMessage(CoapMessage.Type.CON, code, mid, new byte[] {0x5a}, options, payload);

        Main.Identity id = Main.Identity.loadOrCreate(Path.of("state/coordinator.properties"));
        LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();
        Reassembler reassembler = new Reassembler();
        long t0 = System.nanoTime();
        try (HostLink link = new HostLink(new SerialTransport(args[0]), events::add)) {
            link.ping();
            link.configure(id.eui(), id.networkKey(), id.counterFloor(), id.channelKhz());
            var accepted = link.tx(TxRequest.unicast(dev, TxRequest.TRAIN_ANCHORED,
                    Datagram.udp(id.eui(), dev, BRIDGE_PORT, Datagram.COAP_PORT, request.encode())));
            System.out.printf("sent %s %s MID %04x (%s)%n", args[2], args[3], mid, accepted);
            long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(waitSeconds);
            while (System.nanoTime() < end) {
                Event e = events.poll(200, TimeUnit.MILLISECONDS);
                double t = (System.nanoTime() - t0) / 1e9;
                if (e instanceof Event.TxDone d) {
                    System.out.printf("%6.3f s  %s%n", t, d);
                } else if (e instanceof Event.Rx rx && rx.src().equals(dev)) {
                    Optional<byte[]> whole = reassembler.add(rx.src(), rx.packet(), java.time.Instant.now());
                    if (whole.isEmpty()) {
                        continue;
                    }
                    Datagram d = Datagram.parse(whole.get(), rx.src(), rx.broadcast() ? Optional.empty() : Optional.of(id.eui()));
                    if (d.nextHeader() != tadocoord.lowpan.Ipv6.NH_UDP) {
                        System.out.printf("%6.3f s  non-UDP from device (%d bytes)%n", t, whole.get().length);
                        continue;
                    }
                    CoapMessage m = CoapMessage.decode(d.payload());
                    String mark = m.mid() == mid ? "  <== our answer" : "";
                    System.out.printf("%6.3f s  %s %s %s MID %04x payload %s%s%n", t, m.type(),
                            CoapMessage.codeString(m.code()), m.path(), m.mid(),
                            HexFormat.of().formatHex(m.payload()), mark);
                }
            }
        }
    }
}
