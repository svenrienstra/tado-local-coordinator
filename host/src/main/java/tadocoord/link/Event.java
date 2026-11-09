package tadocoord.link;

import java.nio.BufferUnderflowException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import tadocoord.Eui;

/** Unsolicited events from the CC1312 (docs/HOSTLINK.md). */
public sealed interface Event {
    int LOG = 0xc0;
    int RX = 0xc1;
    int BEACON_ANSWERED = 0xc2;
    int RX_UNVERIFIED = 0xc3;
    int TX_DONE = 0xc4;

    record Log(String text) implements Event {}

    /** {@code RX} flag bits (docs/HOSTLINK.md). */
    int RX_PAIRING_KEY = 1;
    int RX_BROADCAST = 1 << 1;
    int RX_ACKED = 1 << 2;

    /** A verified Data frame; {@code packet} is the 6LoWPAN datagram. */
    record Rx(boolean pairingKey, boolean broadcast, boolean acked, int rssi, int macSeq, Eui src,
            long counter, long rat, byte[] packet) implements Event {}

    record BeaconAnswered(Eui joiner, long requestCounter, int rssi, int beaconSeq) implements Event {}

    record RxUnverified(int rssi, long rat, byte[] frame) implements Event {}

    record TxDone(int handle, Result result) implements Event {
        public enum Result {
            ACKED(0), SENT(1), NO_ACK(2), RADIO_ERROR(3);

            private final int wire;

            Result(int wire) {
                this.wire = wire;
            }

            /** An unknown code from a newer firmware reads as an error, not as success. */
            static Result fromWire(int code) {
                for (Result r : values()) {
                    if (r.wire == code) {
                        return r;
                    }
                }
                return RADIO_ERROR;
            }
        }
    }

    /** Unknown event types are kept, not dropped, so a newer firmware stays visible. */
    record Unknown(int type, byte[] payload) implements Event {}

    /** Parses an event; a payload too short for its type comes back as {@link Unknown}. */
    static Event parse(int type, byte[] p) {
        try {
            return parseStrict(type, p);
        } catch (BufferUnderflowException e) {
            return new Unknown(type, Arrays.copyOf(p, p.length));
        }
    }

    private static Event parseStrict(int type, byte[] p) {
        ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
        return switch (type) {
            case LOG -> new Log(new String(p, StandardCharsets.US_ASCII));
            case RX -> {
                int flags = b.get() & 0xff;
                int rssi = b.get();
                int seq = b.get() & 0xff;
                Eui src = new Eui(bytes(b, 8));
                long counter = b.getInt() & 0xffffffffL;
                long rat = b.getInt() & 0xffffffffL;
                yield new Rx((flags & RX_PAIRING_KEY) != 0, (flags & RX_BROADCAST) != 0, (flags & RX_ACKED) != 0,
                        rssi, seq, src, counter, rat, bytes(b, b.remaining()));
            }
            case BEACON_ANSWERED -> new BeaconAnswered(new Eui(bytes(b, 8)), b.getInt() & 0xffffffffL,
                    b.get(), b.get() & 0xff);
            case RX_UNVERIFIED -> new RxUnverified(b.get(), b.getInt() & 0xffffffffL, bytes(b, b.remaining()));
            case TX_DONE -> new TxDone(b.get() & 0xff, TxDone.Result.fromWire(b.get() & 0xff));
            default -> new Unknown(type, Arrays.copyOf(p, p.length));
        };
    }

    private static byte[] bytes(ByteBuffer b, int n) {
        byte[] out = new byte[n];
        b.get(out);
        return out;
    }
}
