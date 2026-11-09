package tadocoord;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.FileAttribute;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.List;
import java.util.Properties;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import tadocoord.join.JoinSession;
import tadocoord.link.Event;
import tadocoord.link.HostLink;
import tadocoord.link.LinkCapture;
import tadocoord.link.StatsDelta;
import tadocoord.link.SerialTransport;

/**
 * M-1 / M2 bench tool: {@code join --port /dev/tty.usbmodemXXXX [--state DIR] [--window SECONDS]}.
 *
 * <p>Opens a pair window on the CC1312 and walks one button-reset device through the join.
 * Exits 0 when the device answers {@code d/pair} under our network key.
 */
public final class Main {
    static final int CHANNEL_KHZ = 868_300;
    /** Counter values reserved per run, as freedo does: never reuse one after a restart. */
    static final long COUNTER_BLOCK = 4096;
    static final Duration STATS_INTERVAL = Duration.ofSeconds(5);
    private static final DateTimeFormatter FILE_TIME =
            DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss").withZone(ZoneOffset.UTC);

    public static void main(String[] args) throws Exception {
        if (args.length < 1 || !args[0].equals("join")) {
            usage();
        }
        String port = null;
        Path stateDir = Path.of("state");
        int window = 120;
        for (int i = 1; i + 1 < args.length; i += 2) {
            switch (args[i]) {
                case "--port" -> port = args[i + 1];
                case "--state" -> stateDir = Path.of(args[i + 1]);
                case "--window" -> window = Integer.parseInt(args[i + 1]);
                default -> usage();
            }
        }
        if (port == null) {
            usage();
        }
        System.exit(join(port, stateDir, window));
    }

    private static int join(String port, Path stateDir, int windowSeconds) throws Exception {
        Identity id = Identity.loadOrCreate(stateDir.resolve("coordinator.properties"));
        log("coordinator " + id.eui + ", counter floor " + id.counterFloor);

        LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();
        Path capture = stateDir.resolve("captures/hostlink-" + FILE_TIME.format(Instant.now()) + ".log");
        log("capturing the host link to " + capture);
        try (LinkCapture tap = new LinkCapture(capture, Clock.systemUTC(), List.of(id.networkKey));
                HostLink link = new HostLink(new SerialTransport(port), events::add, tap)) {
            HostLink.Pong pong = link.ping();
            log("firmware v" + pong.firmwareVersion());
            link.configure(id.eui, id.networkKey, id.counterFloor, CHANNEL_KHZ);

            JoinSession session = new JoinSession(id.eui, id.networkKey, link, Clock.systemUTC(),
                    new SecureRandom(), Main::log);
            link.pairWindow(windowSeconds);
            log("pair window open for " + windowSeconds + " s: long-press the device button now");
            try {
                runWindow(link, session, events, windowSeconds);
            } finally {
                closeWindow(link);
            }
            return session.state() == JoinSession.State.JOINED ? 0 : 1;
        }
    }

    /** Coordinator EUI, network key and counter floor, persisted across runs. */
    record Identity(Eui eui, byte[] networkKey, long counterFloor) {
        static Identity loadOrCreate(Path file) throws IOException {
            Properties p = new Properties();
            if (Files.exists(file)) {
                try (InputStream in = Files.newInputStream(file)) {
                    p.load(in);
                }
            } else {
                SecureRandom rng = new SecureRandom();
                byte[] key = new byte[16];
                rng.nextBytes(key);
                p.setProperty("coordinator_eui", Eui.randomCoordinator(rng).toString());
                p.setProperty("network_key", Hex.of(key));
                p.setProperty("counter_floor", "1");
            }
            Identity id = new Identity(Eui.ofHex(p.getProperty("coordinator_eui")),
                    Hex.parse(p.getProperty("network_key")), Long.parseLong(p.getProperty("counter_floor")));
            // Reserve this run's block before the first frame goes out.
            p.setProperty("counter_floor", Long.toString(id.counterFloor + COUNTER_BLOCK));
            // Owner-only from creation: the key must never sit in a readable file, even briefly.
            if (Files.notExists(file.getParent())) {
                Files.createDirectories(file.getParent(), ownerOnly("rwx------"));
            }
            Path tmp = file.resolveSibling(file.getFileName() + ".tmp");
            Files.deleteIfExists(tmp);
            Files.createFile(tmp, ownerOnly("rw-------"));
            try (OutputStream out = Files.newOutputStream(tmp)) {
                p.store(out, "tado-coordinator identity: holds the network key, keep private");
            }
            Files.move(tmp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            return id;
        }

        private static FileAttribute<?>[] ownerOnly(String perms) {
            boolean posix = FileSystems.getDefault().supportedFileAttributeViews().contains("posix");
            return posix ? new FileAttribute<?>[] {PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString(perms))}
                    : new FileAttribute<?>[0];
        }
    }

    private static void runWindow(HostLink link, JoinSession session, LinkedBlockingQueue<Event> events,
            int windowSeconds) throws IOException, InterruptedException {
        Instant end = Instant.now().plusSeconds(windowSeconds);
        StatsDelta stats = new StatsDelta();
        boolean statsSupported = true;
        Instant nextStats = Instant.now();
        while (Instant.now().isBefore(end) && session.state() != JoinSession.State.FAILED) {
            if (statsSupported && !Instant.now().isBefore(nextStats)) {
                statsSupported = pollStats(link, stats);
                nextStats = Instant.now().plus(STATS_INTERVAL);
            }
            try {
                Event e = events.poll(250, TimeUnit.MILLISECONDS);
                if (e != null) {
                    session.onEvent(e);
                }
                session.tick();
            } catch (HostLink.LinkException e) {
                // The CC1312 refused one request (e.g. still busy after retries); the session goes on.
                log("cc1312 refused a request: " + e.getMessage());
            }
            if (session.state() == JoinSession.State.JOINED) {
                // Stay a little longer to serve auth/token and time (M2 steps 8-10).
                end = min(end, Instant.now().plusSeconds(30));
            }
        }
    }

    /** Never leave the CC1312 answering beacon requests under the shared pairing key. */
    private static void closeWindow(HostLink link) {
        try {
            link.pairWindow(0);
        } catch (IOException e) {
            log("could not close the pair window (" + e.getMessage() + "); the CC1312 closes it at its deadline");
        }
    }

    /** Logs the CC1312 counters that moved; false when the firmware has no STATS. */
    private static boolean pollStats(HostLink link, StatsDelta stats) throws IOException {
        try {
            String moved = stats.next(link.stats());
            if (!moved.isEmpty()) {
                log("cc1312 stats: " + moved);
            }
            return true;
        } catch (HostLink.LinkException e) {
            log("firmware has no STATS (status 0x%02x); not polling".formatted(e.status()));
            return false;
        }
    }

    private static Instant min(Instant a, Instant b) {
        return a.isBefore(b) ? a : b;
    }

    private static void log(String s) {
        System.out.println(Instant.now() + "  " + s);
    }

    private static void usage() {
        System.err.println("usage: join --port <serial device> [--state <dir>] [--window <seconds>]");
        System.exit(2);
    }
}
