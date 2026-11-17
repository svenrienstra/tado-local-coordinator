package tadocoord.bench;

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HexFormat;
import tadocoord.Main;
import tadocoord.link.Event;
import tadocoord.link.HostLink;
import tadocoord.link.SerialTransport;

/**
 * Logs every frame the CC1312 reports, one line each: {@code <rat_end> <rssi> U <hex>} for a frame
 * it could not open, {@code <rat_end> <rssi> R <src> seq=<n>} for one it could. {@code rat_end} is
 * the RAT (4 MHz) time the frame ended on air. Flash a {@code make SNIFF=1} build first, or the
 * firmware rate-limits the frames it cannot open. Feed the file to {@code tools/sniff/*.py}.
 *
 * <p>{@code SniffBench <port> <out file> <seconds>}
 */
public final class SniffBench {
    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            System.err.println("usage: SniffBench <port> <out file> <seconds>");
            System.exit(2);
        }
        Main.Identity id = Main.Identity.loadOrCreate(Path.of("state/coordinator.properties"));
        HexFormat hex = HexFormat.of();
        try (PrintWriter out = new PrintWriter(Files.newBufferedWriter(Path.of(args[1])), true);
                HostLink link = new HostLink(new SerialTransport(args[0]), e -> {
                    switch (e) {
                        case Event.RxUnverified u -> out.printf("%d %d U %s%n", u.rat(), u.rssi(), hex.formatHex(u.frame()));
                        case Event.Rx r -> out.printf("%d %d R %s seq=%d%n", r.rat(), r.rssi(), r.src(), r.macSeq());
                        default -> { }
                    }
                })) {
            link.ping();
            link.configure(id.eui(), id.networkKey(), id.counterFloor(), id.channelKhz());
            Thread.sleep(Long.parseLong(args[2]) * 1000);
            System.out.println("stats " + Arrays.toString(link.stats()));
        }
    }
}
