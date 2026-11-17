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
import java.util.Map;
import java.util.Properties;
import java.util.TreeMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import tadocoord.join.Installation;
import tadocoord.join.JoinSession;
import tadocoord.link.Event;
import tadocoord.link.HostLink;
import tadocoord.link.LinkCapture;
import tadocoord.link.StatsDelta;
import tadocoord.link.SerialTransport;

/**
 * M-1 / M2 bench tool.
 *
 * <ul>
 *   <li>{@code join --port /dev/tty.usbserial-XXXX [--state DIR] [--window SECONDS]} opens a pair
 *       window on the CC1312 and walks one button-reset device through the join. Exits 0 when the
 *       device answers {@code d/pair} under our network key.
 *   <li>{@code serve --port ... [--state DIR]} opens no pair window and runs until stopped: it
 *       keeps the joined devices served (Router Advertisements, reports, {@code time}).
 * </ul>
 */
public final class Main {
    /**
     * The middle of the band this board receives the plant cleanly in (868.290-868.385 MHz,
     * measured 2026-11-17); it includes the board's crystal error.
     */
    public static final int CHANNEL_KHZ = 868_335;
    /** Counter values reserved per run, as freedo does: never reuse one after a restart. */
    static final long COUNTER_BLOCK = 4096;
    /** serve runs for days; this outlasts that at a frame a second (32-bit counter: 4096 such runs). */
    static final long SERVE_COUNTER_BLOCK = 1L << 20;
    static final Duration STATS_INTERVAL = Duration.ofSeconds(5);
    private static final DateTimeFormatter FILE_TIME =
            DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss").withZone(ZoneOffset.UTC);

    public static void main(String[] args) throws Exception {
        if (args.length < 1 || !(args[0].equals("join") || args[0].equals("serve"))) {
            usage();
        }
        String port = null;
        Path stateDir = Path.of("state");
        int window = 120;
        Integer channelKhz = null; // the tuned one from state/, else CHANNEL_KHZ
        Map<Integer, Eui> zones = new TreeMap<>();
        Map<Integer, Integer> setpoints = new TreeMap<>();
        for (int i = 1; i + 1 < args.length; i += 2) {
            switch (args[i]) {
                case "--port" -> port = args[i + 1];
                case "--state" -> stateDir = Path.of(args[i + 1]);
                case "--window" -> window = Integer.parseInt(args[i + 1]);
                case "--channel" -> channelKhz = Integer.parseInt(args[i + 1]);
                case "--zone" -> {
                    String[] z = args[i + 1].split("=");
                    zones.put(Integer.parseInt(z[0]), Eui.ofHex(z[1]));
                }
                case "--setpoint" -> {
                    String[] z = args[i + 1].split("=");
                    setpoints.put(Integer.parseInt(z[0]), (int) Math.round(Double.parseDouble(z[1]) * 100));
                }
                default -> usage();
            }
        }
        if (port == null || (args[0].equals("join") && window <= 0)) {
            usage();
        }
        System.exit(run(port, stateDir, args[0].equals("join") ? window : 0, channelKhz, new Installation(0, zones, setpoints)));
    }

    /** With windowSeconds 0, serves without a pair window until stopped. */
    private static int run(String port, Path stateDir, int windowSeconds, Integer channelOverride,
            Installation zones) throws Exception {
        Path identityFile = stateDir.resolve("coordinator.properties");
        Identity id = Identity.loadOrCreate(identityFile,
                windowSeconds == 0 ? SERVE_COUNTER_BLOCK : COUNTER_BLOCK);
        log("coordinator " + id.eui + ", counter floor " + id.counterFloor);

        LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();
        Path capture = stateDir.resolve("captures/hostlink-" + FILE_TIME.format(Instant.now()) + ".log");
        log("capturing the host link to " + capture);
        try (LinkCapture tap = new LinkCapture(capture, Clock.systemUTC(), List.of(id.networkKey));
                HostLink link = new HostLink(new SerialTransport(port), events::add, tap)) {
            HostLink.Pong pong = link.ping();
            log("firmware v" + pong.firmwareVersion());
            int channelKhz = channelOverride != null ? channelOverride : id.channelKhz;
            log("carrier " + channelKhz + " kHz");
            link.configure(id.eui, id.networkKey, id.counterFloor, channelKhz);

            if (windowSeconds == 0) {
                JoinSession session = JoinSession.serving(id.eui, id.networkKey, link, Clock.systemUTC(),
                        new SecureRandom(), Main::log)
                        .installation(new Installation(id.homeId, zones.zoneOwners(), zones.setpoints()));
                log("serving the joined devices (home " + id.homeId + ", zones " + zones.zoneOwners()
                        + ", setpoints " + zones.setpoints() + "); stop with Ctrl-C");
                runWindow(link, session, events, Instant.MAX, identityFile);
                return 0;
            }
            JoinSession session = new JoinSession(id.eui, id.networkKey, link, Clock.systemUTC(),
                    new SecureRandom(), Main::log);
            link.pairWindow(windowSeconds);
            log("pair window open for " + windowSeconds + " s: long-press the device button now");
            try {
                runWindow(link, session, events, Instant.now().plusSeconds(windowSeconds), identityFile);
            } finally {
                closeWindow(link);
            }
            return session.state() == JoinSession.State.JOINED ? 0 : 1;
        }
    }

    /** Coordinator EUI, network key and counter floor, persisted across runs. */
    /**
     * Coordinator EUI, network key, counter floor and the carrier the CC1312 last tuned to,
     * persisted across runs.
     */
    public record Identity(Eui eui, byte[] networkKey, long counterFloor, int channelKhz, long homeId) {
        public static Identity loadOrCreate(Path file) throws IOException {
            return loadOrCreate(file, COUNTER_BLOCK);
        }

        /** Reserves {@code block} frame counter values for this run. */
        static Identity loadOrCreate(Path file, long block) throws IOException {
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
            if (p.getProperty("home_id") == null) {
                // The home in every h/<home>/... path a device uses; never 0.
                p.setProperty("home_id", Long.toString(1 + new SecureRandom().nextInt(Integer.MAX_VALUE - 1)));
            }
            Identity id = new Identity(Eui.ofHex(p.getProperty("coordinator_eui")),
                    Hex.parse(p.getProperty("network_key")), Long.parseLong(p.getProperty("counter_floor")),
                    Integer.parseInt(p.getProperty("channel_khz", Integer.toString(CHANNEL_KHZ))),
                    Long.parseLong(p.getProperty("home_id")));
            // Reserve this run's block before the first frame goes out.
            p.setProperty("counter_floor", Long.toString(id.counterFloor + block));
            store(file, p);
            return id;
        }

        /** Keeps the carrier the CC1312 tuned to, so the next run starts on it. */
        public static void saveChannel(Path file, int khz) throws IOException {
            Properties p = new Properties();
            try (InputStream in = Files.newInputStream(file)) {
                p.load(in);
            }
            p.setProperty("channel_khz", Integer.toString(khz));
            store(file, p);
        }

        private static void store(Path file, Properties p) throws IOException {
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
        }

        private static FileAttribute<?>[] ownerOnly(String perms) {
            boolean posix = FileSystems.getDefault().supportedFileAttributeViews().contains("posix");
            return posix ? new FileAttribute<?>[] {PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString(perms))}
                    : new FileAttribute<?>[0];
        }
    }

    private static void runWindow(HostLink link, JoinSession session, LinkedBlockingQueue<Event> events,
            Instant end, Path identityFile) throws IOException, InterruptedException {
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
                    keepRetune(e, identityFile);
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

    private static final Pattern RETUNED = Pattern.compile("retuned to (\\d+) kHz");

    /** The CC1312 stores nothing: the carrier it followed is kept for the next CONFIG. */
    private static void keepRetune(Event e, Path identityFile) throws IOException {
        if (e instanceof Event.Log l) {
            Matcher m = RETUNED.matcher(l.text());
            if (m.find()) {
                Identity.saveChannel(identityFile, Integer.parseInt(m.group(1)));
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
        System.err.println("usage: join --port <serial device> [--state <dir>] [--window <seconds>] [--channel <kHz>]");
        System.err.println("       serve --port <serial device> [--state <dir>] [--channel <kHz>] [--zone <n>=<owner EUI>]... [--setpoint <n>=<C>]...");
        System.exit(2);
    }
}
