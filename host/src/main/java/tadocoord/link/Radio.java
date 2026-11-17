package tadocoord.link;

import java.io.IOException;
import java.util.Optional;
import tadocoord.Eui;

/** What the coordinator logic needs from the CC1312: queue a frame for transmission. */
public interface Radio {

    /**
     * A frame to send. The CC1312 allocates MAC seq and counter, wraps and seals the packet.
     *
     * @param flags {@link #PAIRING_KEY}, {@link #FRAME_PENDING}, {@link #TRAIN_FULL} or
     *     {@link #TRAIN_ANCHORED}
     * @param dst empty for a broadcast
     */
    record TxRequest(int flags, Optional<Eui> dst, byte[] packet) {
        public static final int PAIRING_KEY = 1;
        public static final int FRAME_PENDING = 1 << 2;
        public static final int TRAIN_FULL = 1 << 4;
        /**
         * A short train on the device's channel sample, which the CC1312 learns from the
         * device's Enhanced ACKs; a full train while it knows none, or after a miss.
         */
        public static final int TRAIN_ANCHORED = 2 << 4;

        public static TxRequest unicast(Eui dst, int flags, byte[] packet) {
            return new TxRequest(flags, Optional.of(dst), packet);
        }

        public static TxRequest broadcast(int flags, byte[] packet) {
            return new TxRequest(flags, Optional.empty(), packet);
        }
    }

    record TxAccepted(int handle, int macSeq, long counter) {}

    TxAccepted tx(TxRequest request) throws IOException;
}
