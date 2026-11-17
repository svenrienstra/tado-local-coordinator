package tadocoord;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
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
}
