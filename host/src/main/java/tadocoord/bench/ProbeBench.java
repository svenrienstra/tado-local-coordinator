package tadocoord.bench;

import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import tadocoord.Eui;
import tadocoord.Main;
import tadocoord.coap.CoapMessage;
import tadocoord.link.Event;
import tadocoord.link.HostLink;
import tadocoord.link.Radio.TxRequest;
import tadocoord.link.SerialTransport;
import tadocoord.lowpan.Datagram;

/**
 * Probes the downlink to one joined device: sends {@code GET d/fw/state} a number of times, 3 s
 * apart, the first behind a full train and the rest anchored, and prints each MAC outcome, the
 * device's frames and, at the end, the CC1312's counters ({@code jobs_anchored} is one of them).
 *
 * <p>{@code ProbeBench <port> <device EUI> <count>}
 */
public final class ProbeBench {
    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            System.err.println("usage: ProbeBench <port> <device EUI> <count>");
            System.exit(2);
        }
        Main.Identity id = Main.Identity.loadOrCreate(Path.of("state/coordinator.properties"));
        Eui dev = Eui.ofHex(args[1]);
        long t0 = System.nanoTime();
        try (HostLink link = new HostLink(new SerialTransport(args[0]), e -> {
            if (e instanceof Event.TxDone || e instanceof Event.Log
                    || (e instanceof Event.Rx r && r.src().equals(dev))) {
                System.out.printf("%7.3f s  %s%n", (System.nanoTime() - t0) / 1e9, e);
            }
        })) {
            link.ping();
            link.configure(id.eui(), id.networkKey(), id.counterFloor(), id.channelKhz());
            for (int i = 0; i < Integer.parseInt(args[2]); i++) {
                CoapMessage get = new CoapMessage(CoapMessage.Type.CON, CoapMessage.code(0, 1), 0x7000 + i,
                        new byte[] {(byte) i}, List.of(CoapMessage.path("d"), CoapMessage.path("fw"),
                        CoapMessage.path("state")), new byte[0]);
                int train = i == 0 ? TxRequest.TRAIN_FULL : TxRequest.TRAIN_ANCHORED;
                var accepted = link.tx(TxRequest.unicast(dev, train,
                        Datagram.udp(id.eui(), dev, Datagram.COAP_PORT, Datagram.COAP_PORT, get.encode())));
                System.out.printf("%7.3f s  queued %s%n", (System.nanoTime() - t0) / 1e9, accepted);
                Thread.sleep(3000);
            }
            System.out.println("stats " + Arrays.toString(link.stats()));
        }
    }
}
