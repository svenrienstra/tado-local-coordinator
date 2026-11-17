package tadocoord.bench;

import java.nio.file.Path;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.atomic.AtomicReference;
import tadocoord.Main;
import tadocoord.link.Event;
import tadocoord.link.HostLink;
import tadocoord.link.SerialTransport;

/**
 * Finds the carrier frequency the plant is heard on: receives at each frequency for a while, in
 * several interleaved rounds, and counts good-CRC and bad-CRC frames (the CC1312's {@code rx_ok}
 * and {@code rx_crc_fail}). Works best with plenty of traffic on air, such as the commercial bridge's.
 * The middle of the clean band is the frequency to use; on our first board it was 868.335 MHz.
 * Sends nothing secured, so it spends no frame counters.
 *
 * <p>{@code RxSweep <port> <seconds per step> <rounds> <kHz>...}
 */
public final class RxSweep {
    public static void main(String[] args) throws Exception {
        if (args.length < 4) {
            System.err.println("usage: RxSweep <port> <seconds per step> <rounds> <kHz>...");
            System.exit(2);
        }
        Main.Identity id = Main.Identity.loadOrCreate(Path.of("state/coordinator.properties"));
        int dwell = Integer.parseInt(args[1]);
        int rounds = Integer.parseInt(args[2]);
        AtomicReference<long[]> rssi = new AtomicReference<>(new long[2]); // sum, count
        Map<Integer, long[]> total = new TreeMap<>(); // ok, crc_fail, rssi sum, rssi count
        try (HostLink link = new HostLink(new SerialTransport(args[0]), e -> {
            if (e instanceof Event.RxUnverified u) {
                rssi.get()[0] += u.rssi();
                rssi.get()[1]++;
            }
        })) {
            link.ping();
            for (int r = 0; r < rounds; r++) {
                for (int i = 3; i < args.length; i++) {
                    int khz = Integer.parseInt(args[i]);
                    link.configure(id.eui(), id.networkKey(), id.counterFloor(), khz);
                    long[] before = link.stats();
                    rssi.set(new long[2]);
                    Thread.sleep(dwell * 1000L);
                    long[] after = link.stats();
                    long[] t = total.computeIfAbsent(khz, k -> new long[4]);
                    t[0] += after[0] - before[0];
                    t[1] += after[1] - before[1];
                    t[2] += rssi.get()[0];
                    t[3] += rssi.get()[1];
                }
            }
        }
        System.out.println("kHz      ok  crc_fail  bad%   mean_rssi");
        total.forEach((k, t) -> System.out.printf("%d  %5d  %8d  %5.1f  %6.1f%n", k, t[0], t[1],
                100.0 * t[1] / Math.max(1, t[0] + t[1]), t[3] == 0 ? 0.0 : (double) t[2] / t[3]));
    }
}
