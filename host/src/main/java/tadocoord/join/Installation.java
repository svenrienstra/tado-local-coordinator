package tadocoord.join;

import java.util.Map;
import tadocoord.Eui;

/**
 * What the coordinator serves its devices about the home: the home id of every {@code
 * h/<home>/...} path, each zone's owner (the device that measures for it) and each zone's
 * setpoint in centi-degrees C (DEFAULT_SETPOINT when not given).
 */
public record Installation(long homeId, Map<Integer, Eui> zoneOwners, Map<Integer, Integer> setpoints) {
    public static final Installation NONE = new Installation(0, Map.of(), Map.of());
    public static final int DEFAULT_SETPOINT = 1800;

    public Installation {
        zoneOwners = Map.copyOf(zoneOwners);
        setpoints = Map.copyOf(setpoints);
    }

    public int setpoint(int zone) {
        return setpoints.getOrDefault(zone, DEFAULT_SETPOINT);
    }
}
