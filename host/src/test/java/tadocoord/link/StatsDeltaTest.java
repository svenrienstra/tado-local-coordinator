package tadocoord.link;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class StatsDeltaTest {

    @Test
    void reportsOnlyTheCountersThatMoved() {
        StatsDelta d = new StatsDelta();
        assertEquals("rx_ok +5 rx_crc_fail +2", d.next(new long[] {5, 2, 0}));
        assertEquals("", d.next(new long[] {5, 2, 0}));
        assertEquals("rx_crc_fail +40", d.next(new long[] {5, 42, 0}));
    }

    @Test
    void handlesU32WrapAndCountersANewerFirmwareAppends() {
        StatsDelta d = new StatsDelta();
        d.next(new long[] {0xffff_fffeL});
        long[] next = new long[15];
        next[0] = 1;
        next[14] = 9;
        assertEquals("rx_ok +3 counter14 +9", d.next(next));
    }
}
