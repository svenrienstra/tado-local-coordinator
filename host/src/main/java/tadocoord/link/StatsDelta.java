package tadocoord.link;

import java.util.ArrayList;
import java.util.List;

/** Turns successive STATS replies into a line naming only the counters that moved. */
public final class StatsDelta {
    /** docs/HOSTLINK.md STATS order; counters a newer firmware appends show by index. */
    static final List<String> NAMES = List.of("rx_ok", "rx_crc_fail", "rx_unverified", "beacons_answered",
            "acks_sent", "answers_refused", "jobs_ok", "jobs_failed", "jobs_timed_out", "radio_errors",
            "link_crc_errors", "link_resyncs", "acks_late", "jobs_anchored", "rx_duplicates");

    private long[] last = new long[0];

    /** The counters that changed since the previous call, e.g. {@code "rx_ok +12 rx_crc_fail +3"}. */
    public String next(long[] counters) {
        List<String> moved = new ArrayList<>();
        for (int i = 0; i < counters.length; i++) {
            long before = i < last.length ? last[i] : 0;
            long delta = (counters[i] - before) & 0xffffffffL; // u32 counters wrap
            if (delta != 0) {
                moved.add((i < NAMES.size() ? NAMES.get(i) : "counter" + i) + " +" + delta);
            }
        }
        last = counters.clone();
        return String.join(" ", moved);
    }
}
