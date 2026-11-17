package tadocoord.join;

import java.util.Map;
import tadocoord.Eui;

/**
 * What the coordinator serves its devices about the home: the home id of every {@code
 * h/<home>/...} path, and each zone's owner, the device that measures for it.
 */
public record Installation(long homeId, Map<Integer, Eui> zoneOwners) {
    public static final Installation NONE = new Installation(0, Map.of());

    public Installation {
        zoneOwners = Map.copyOf(zoneOwners);
    }
}
