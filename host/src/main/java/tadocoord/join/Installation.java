package tadocoord.join;

import java.time.Duration;
import java.util.Map;
import tadocoord.Eui;

/**
 * What the coordinator serves its devices about the home: the home id of every {@code
 * h/<home>/...} path, each zone's owner (the device that measures for it), each zone's
 * setpoint in centi-degrees C (DEFAULT_SETPOINT when not given), and how long a setpoint set on
 * a device lasts before the zone returns to its own ({@link Duration#ZERO}: until changed). The
 * device reports such an overlay as manual; its end is the coordinator's to keep, as in tado.
 */
public record Installation(long homeId, Map<Integer, Eui> zoneOwners, Map<Integer, Integer> setpoints,
        Duration deviceOverlay) {
    public static final int DEFAULT_SETPOINT = 1800;
    /** An hour, as the tado app offers for a manual change. */
    public static final Duration DEFAULT_DEVICE_OVERLAY = Duration.ofHours(1);
    public static final Installation NONE = new Installation(0, Map.of(), Map.of());

    public Installation {
        zoneOwners = Map.copyOf(zoneOwners);
        setpoints = Map.copyOf(setpoints);
        if (deviceOverlay.isNegative()) {
            throw new IllegalArgumentException("device overlay duration must not be negative");
        }
    }

    public Installation(long homeId, Map<Integer, Eui> zoneOwners, Map<Integer, Integer> setpoints) {
        this(homeId, zoneOwners, setpoints, DEFAULT_DEVICE_OVERLAY);
    }

    public int setpoint(int zone) {
        return setpoints.getOrDefault(zone, DEFAULT_SETPOINT);
    }
}
