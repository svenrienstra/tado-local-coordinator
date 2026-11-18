package tadocoord;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class MainIdentityTest {

    @Test
    void identityIsOwnerOnlyAndReservesACounterBlockPerRun(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("state/coordinator.properties");

        Main.Identity first = Main.Identity.loadOrCreate(file);
        Main.Identity second = Main.Identity.loadOrCreate(file);

        assertEquals("rw-------", PosixFilePermissions.toString(Files.getPosixFilePermissions(file)));
        assertEquals("rwx------", PosixFilePermissions.toString(Files.getPosixFilePermissions(file.getParent())));
        assertTrue(first.eui().hasTadoSuffix());
        assertEquals(first.eui(), second.eui());
        assertArrayEquals(first.networkKey(), second.networkKey());
        assertEquals(first.counterFloor() + Main.COUNTER_BLOCK, second.counterFloor());
    }

    @Test
    void keepsTheCarrierTheRadioTunedToAcrossRuns(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("state/coordinator.properties");
        Main.Identity first = Main.Identity.loadOrCreate(file);
        assertEquals(Main.CHANNEL_KHZ, first.channelKhz());

        Main.Identity.saveChannel(file, 868_340);
        Main.Identity second = Main.Identity.loadOrCreate(file);

        assertEquals(868_340, second.channelKhz());
        assertArrayEquals(first.networkKey(), second.networkKey()); // the rest of the file is kept
        assertEquals("rw-------", PosixFilePermissions.toString(Files.getPosixFilePermissions(file)));
    }

    @Test
    void aKeptCarrierStaysWithinTheTuningRangeOfTheBandMiddle(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("state/coordinator.properties");
        Main.Identity.loadOrCreate(file);
        // Each run may retune 40 kHz from where it started; kept as is, that would add up.
        Main.Identity.saveChannel(file, Main.CHANNEL_KHZ + 70);
        assertEquals(Main.CHANNEL_KHZ + Main.TUNE_MAX_KHZ, Main.Identity.loadOrCreate(file).channelKhz());
    }

    @Test
    void refusesToStartOnceTheFrameCountersAreUsedUp(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("state/coordinator.properties");
        Main.Identity.loadOrCreate(file);
        java.util.Properties p = new java.util.Properties();
        try (var in = Files.newInputStream(file)) {
            p.load(in);
        }
        p.setProperty("counter_floor", Long.toString(Main.COUNTER_LIMIT - Main.COUNTER_BLOCK + 2));
        try (var out = Files.newOutputStream(file)) {
            p.store(out, null);
        }
        assertThrows(IllegalStateException.class, () -> Main.Identity.loadOrCreate(file));
        p.load(Files.newInputStream(file));
        assertEquals(Long.toString(Main.COUNTER_LIMIT - Main.COUNTER_BLOCK + 2), p.getProperty("counter_floor"));
    }
}
