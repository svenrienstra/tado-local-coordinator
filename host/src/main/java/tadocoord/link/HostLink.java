package tadocoord.link;

import java.io.Closeable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;
import tadocoord.Eui;

/**
 * Client side of the host link (docs/HOSTLINK.md): one request in flight at a time, retried
 * under the same seq so the CC1312 replays its cached reply instead of executing twice.
 *
 * <p>Events are delivered on their own thread, so a listener may send requests.
 */
public final class HostLink implements Closeable, Radio {
    public static final int PING = 0x01;
    public static final int CONFIG = 0x02;
    public static final int PAIR_WINDOW = 0x03;
    public static final int TX = 0x04;
    public static final int STATS = 0x05;
    static final int REPLY_FLAG = 0x80;
    static final int EVENT_MASK = 0xc0;
    /** TX flag bit 1; derived from the request having no destination. */
    private static final int TX_BROADCAST = 1 << 1;

    static final long REPLY_TIMEOUT_MS = 200;
    static final int ATTEMPTS = 4;
    /** Reply status: TX queue full. Frees up as queued trains (about 1 s each) leave the radio. */
    static final int STATUS_BUSY = 0x03;
    static final int STATUS_UNSUPPORTED = 0x05;
    static final long BUSY_RETRY_MS = 100;
    static final long BUSY_TIMEOUT_MS = 8_000;

    /** Byte stream to the CC1312: a serial port, or a TCP socket to the ESP bridge later. */
    public interface Transport extends Closeable {
        void write(byte[] b) throws IOException;

        /** Blocks until at least one byte arrives; returns -1 at end of stream. */
        int read(byte[] buf) throws IOException;
    }

    public record Pong(int firmwareVersion, long rat) {}

    /** Sees every frame that crosses the link, both ways, e.g. to capture it to a file. */
    public interface FrameTap {
        void frame(boolean outbound, int seq, int type, byte[] payload);
    }

    private final Transport transport;
    private final Consumer<Event> listener;
    private final FrameTap tap;
    private final ExecutorService events = Executors.newSingleThreadExecutor(r -> daemon(r, "hostlink-events"));
    private final Object requestLock = new Object();
    private volatile Pending pending;
    /** Set when the reader stops; every later request fails with it instead of timing out. */
    private volatile IOException dead;
    /** Random start, so a restarted host does not resend the seq of its last run's request. */
    private int nextSeq = ThreadLocalRandom.current().nextInt(0x100);

    /** The one request awaiting its reply. */
    private record Pending(int seq, CompletableFuture<FrameCodec.Frame> reply) {}

    public HostLink(Transport transport, Consumer<Event> listener) {
        this(transport, listener, (outbound, seq, type, payload) -> { });
    }

    public HostLink(Transport transport, Consumer<Event> listener, FrameTap tap) {
        this.transport = transport;
        this.listener = listener;
        this.tap = tap;
        daemon(this::readLoop, "hostlink-reader").start();
    }

    public Pong ping() throws IOException {
        ByteBuffer b = le(request(PING, new byte[0]));
        return new Pong(b.getShort() & 0xffff, b.getInt() & 0xffffffffL);
    }

    public void configure(Eui coordinator, byte[] networkKey, long counterFloor, int channelKhz) throws IOException {
        if (!coordinator.hasTadoSuffix()) {
            throw new IllegalArgumentException("coordinator EUI must end in c51b00");
        }
        ByteBuffer b = ByteBuffer.allocate(8 + 16 + 4 + 4).order(ByteOrder.LITTLE_ENDIAN);
        b.put(coordinator.wire()).put(networkKey).putInt((int) counterFloor).putInt(channelKhz);
        request(CONFIG, b.array());
    }

    /** The CC1312's counters since boot (docs/HOSTLINK.md, STATS), in wire order. */
    public long[] stats() throws IOException {
        ByteBuffer b = le(request(STATS, new byte[0]));
        long[] counters = new long[b.remaining() / 4];
        for (int i = 0; i < counters.length; i++) {
            counters[i] = b.getInt() & 0xffffffffL;
        }
        return counters;
    }

    public void pairWindow(int seconds) throws IOException {
        request(PAIR_WINDOW, ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN).putShort((short) seconds).array());
    }

    @Override
    public TxAccepted tx(TxRequest r) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(1 + 8 + r.packet().length).order(ByteOrder.LITTLE_ENDIAN);
        int flags = r.dst().isEmpty() ? r.flags() | TX_BROADCAST : r.flags();
        b.put((byte) flags).put(r.dst().map(Eui::wire).orElse(new byte[8])).put(r.packet());
        long giveUp = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(BUSY_TIMEOUT_MS);
        while (true) {
            try {
                ByteBuffer reply = le(request(TX, b.array()));
                return new TxAccepted(reply.get() & 0xff, reply.get() & 0xff, reply.getInt() & 0xffffffffL);
            } catch (LinkException e) {
                if (e.status() != STATUS_BUSY || System.nanoTime() > giveUp) {
                    throw e;
                }
                sleep(BUSY_RETRY_MS);
            }
        }
    }

    /** Sends a request and returns the reply payload after the OK status byte. */
    byte[] request(int type, byte[] payload) throws IOException {
        synchronized (requestLock) {
            if (dead != null) {
                throw new IOException("host link is down", dead);
            }
            int seq = nextSeq++ & 0xff;
            byte[] frame = FrameCodec.encode(seq, type, payload);
            for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
                CompletableFuture<FrameCodec.Frame> f = new CompletableFuture<>();
                pending = new Pending(seq, f);
                tap.frame(true, seq, type, payload);
                transport.write(frame);
                try {
                    FrameCodec.Frame reply = f.get(REPLY_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                    if (reply.type() != (type | REPLY_FLAG)) {
                        throw new IOException("reply type 0x%02x to request 0x%02x".formatted(reply.type(), type));
                    }
                    byte[] p = reply.payload();
                    if (p.length == 0 || p[0] != 0) {
                        throw new LinkException(type, p.length == 0 ? -1 : p[0] & 0xff);
                    }
                    return Arrays.copyOfRange(p, 1, p.length);
                } catch (TimeoutException e) {
                    // retry under the same seq
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new IOException("interrupted", e);
                } catch (ExecutionException e) {
                    throw new IOException("host link is down", e.getCause());
                } finally {
                    pending = null;
                }
            }
            throw new IOException("no reply to request 0x%02x after %d attempts".formatted(type, ATTEMPTS));
        }
    }

    private void readLoop() {
        FrameCodec codec = new FrameCodec();
        byte[] buf = new byte[512];
        IOException cause;
        try {
            int n;
            while ((n = transport.read(buf)) >= 0) {
                for (FrameCodec.Frame f : codec.feed(buf, n)) {
                    tap.frame(false, f.seq(), f.type(), f.payload());
                    Pending p = pending;
                    if ((f.type() & EVENT_MASK) == EVENT_MASK) {
                        Event e = Event.parse(f.type(), f.payload());
                        events.execute(() -> listener.accept(e));
                    } else if (p != null && f.seq() == p.seq()) {
                        p.reply().complete(f);
                    }
                    // Anything else is a late reply to an abandoned attempt.
                }
            }
            cause = new IOException("end of stream");
        } catch (IOException e) {
            cause = e;
        } catch (RuntimeException e) {
            cause = new IOException("host-link reader failed", e);
        }
        dead = cause;
        Pending p = pending;
        if (p != null) {
            p.reply().completeExceptionally(cause);
        }
    }

    private static void sleep(long ms) throws IOException {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("interrupted", e);
        }
    }

    @Override
    public void close() throws IOException {
        transport.close();
        events.shutdown();
    }

    private static ByteBuffer le(byte[] b) {
        return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static Thread daemon(Runnable r, String name) {
        Thread t = new Thread(r, name);
        t.setDaemon(true);
        return t;
    }

    /** The CC1312 answered with a non-OK status. */
    public static final class LinkException extends IOException {
        private final int status;

        LinkException(int type, int status) {
            super("request 0x%02x failed with status 0x%02x%s".formatted(type, status,
                    status == STATUS_UNSUPPORTED ? " (CC13x2 older than rev E: not supported)" : ""));
            this.status = status;
        }

        public int status() {
            return status;
        }
    }
}
