package tadocoord.join;

import tadocoord.Eui;

/** Test access to JoinSession's package-private helpers from other packages. */
public final class JoinSessionAccess {
    private JoinSessionAccess() {}

    public static byte[] sessionId(Eui device) {
        return JoinSession.sessionId(device);
    }
}
