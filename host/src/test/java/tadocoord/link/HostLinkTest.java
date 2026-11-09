package tadocoord.link;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;
import org.junit.jupiter.api.Test;

class HostLinkTest {

    /** A scripted CC1312: answers each written request through {@code reply}, or drops it (null). */
    private static final class FakeDevice implements HostLink.Transport {
        final LinkedBlockingQueue<byte[]> toHost = new LinkedBlockingQueue<>();
        final List<Integer> seqsSeen = new ArrayList<>();
        final FrameCodec codec = new FrameCodec();
        Function<FrameCodec.Frame, byte[]> reply;

        @Override
        public synchronized void write(byte[] b) {
            for (FrameCodec.Frame f : codec.feed(b, b.length)) {
                seqsSeen.add(f.seq());
                byte[] r = reply.apply(f);
                if (r != null) {
                    toHost.add(r);
                }
            }
        }

        @Override
        public int read(byte[] buf) throws IOException {
            try {
                byte[] b = toHost.take();
                if (b.length == 0) {
                    return -1;
                }
                System.arraycopy(b, 0, buf, 0, b.length);
                return b.length;
            } catch (InterruptedException e) {
                throw new IOException(e);
            }
        }

        @Override
        public void close() {
            toHost.add(new byte[0]);
        }
    }

    /** A reply to {@code f}: status byte, then {@code extra}. */
    private static byte[] reply(FrameCodec.Frame f, int status, int... extra) {
        byte[] p = new byte[1 + extra.length];
        p[0] = (byte) status;
        for (int i = 0; i < extra.length; i++) {
            p[1 + i] = (byte) extra[i];
        }
        return FrameCodec.encode(f.seq(), f.type() | HostLink.REPLY_FLAG, p);
    }

    @Test
    void aLostReplyIsRetriedUnderTheSameSeq() throws Exception {
        FakeDevice dev = new FakeDevice();
        int[] calls = {0};
        dev.reply = f -> calls[0]++ == 0 ? null : reply(f, 0, 2, 0, 1, 0, 0, 0); // PONG v2, rat 1
        try (HostLink link = new HostLink(dev, e -> { })) {
            HostLink.Pong pong = link.ping();

            assertEquals(2, pong.firmwareVersion());
            assertEquals(2, dev.seqsSeen.size());
            assertEquals(dev.seqsSeen.get(0), dev.seqsSeen.get(1));
        }
    }

    @Test
    void theTapSeesBothDirections() throws Exception {
        FakeDevice dev = new FakeDevice();
        dev.reply = f -> reply(f, 0, 2, 0, 1, 0, 0, 0);
        List<String> seen = new java.util.concurrent.CopyOnWriteArrayList<>();
        try (HostLink link = new HostLink(dev, e -> { },
                (out, seq, type, payload) -> seen.add((out ? ">" : "<") + Integer.toHexString(type)))) {
            link.ping();
        }
        assertEquals(List.of(">1", "<81"), seen);
    }

    @Test
    void aBusyTxQueueIsRetriedUntilItFrees() throws Exception {
        FakeDevice dev = new FakeDevice();
        int[] calls = {0};
        dev.reply = f -> calls[0]++ < 2 ? reply(f, HostLink.STATUS_BUSY) : reply(f, 0, 5, 9, 1, 0, 0, 0);
        try (HostLink link = new HostLink(dev, e -> { })) {
            Radio.TxAccepted accepted = link.tx(Radio.TxRequest.broadcast(0, new byte[] {1}));
            assertEquals(5, accepted.handle());
            assertEquals(3, calls[0]);
        }
    }

    @Test
    void aDeadLinkFailsRequestsAtOnceWithItsCause() throws Exception {
        FakeDevice dev = new FakeDevice();
        dev.reply = f -> reply(f, 0);
        try (HostLink link = new HostLink(dev, e -> { })) {
            dev.toHost.add(new byte[0]); // end of stream
            Thread.sleep(50);
            long start = System.nanoTime();
            IOException e = assertThrows(IOException.class, link::ping);
            assertTrue(System.nanoTime() - start < TimeUnit.MILLISECONDS.toNanos(HostLink.REPLY_TIMEOUT_MS));
            assertEquals("end of stream", e.getCause().getMessage());
        }
    }

    @Test
    void aNonOkStatusIsAnError() throws Exception {
        FakeDevice dev = new FakeDevice();
        dev.reply = f -> reply(f, 0x02); // not configured
        try (HostLink link = new HostLink(dev, e -> { })) {
            HostLink.LinkException e = assertThrows(HostLink.LinkException.class, () -> link.pairWindow(60));
            assertEquals(0x02, e.status());
        }
    }

    @Test
    void aMalformedEventDoesNotStopTheReader() throws Exception {
        FakeDevice dev = new FakeDevice();
        dev.reply = f -> reply(f, 0);
        LinkedBlockingQueue<Event> events = new LinkedBlockingQueue<>();
        try (HostLink link = new HostLink(dev, events::add)) {
            dev.toHost.add(FrameCodec.encode(1, Event.TX_DONE, new byte[] {7})); // one byte short
            dev.toHost.add(FrameCodec.encode(2, Event.LOG, "still here".getBytes()));

            assertTrue(events.poll(2, TimeUnit.SECONDS) instanceof Event.Unknown);
            assertEquals(new Event.Log("still here"), events.poll(2, TimeUnit.SECONDS));
            link.pairWindow(0); // requests still work
        }
    }

    @Test
    void anEventListenerMaySendRequests() throws Exception {
        FakeDevice dev = new FakeDevice();
        dev.reply = f -> reply(f, 0, 1, 0, 0, 0, 0, 0);
        CompletableFuture<HostLink.Pong> fromListener = new CompletableFuture<>();
        HostLink[] holder = new HostLink[1];
        try (HostLink link = new HostLink(dev, e -> {
            try {
                fromListener.complete(holder[0].ping());
            } catch (IOException ex) {
                fromListener.completeExceptionally(ex);
            }
        })) {
            holder[0] = link;
            dev.toHost.add(FrameCodec.encode(9, Event.LOG, "boot".getBytes()));

            assertEquals(1, fromListener.get(2, TimeUnit.SECONDS).firmwareVersion());
            assertTrue(dev.seqsSeen.size() >= 1);
        }
    }
}
