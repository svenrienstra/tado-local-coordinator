package tadocoord;

import java.util.HexFormat;

/** Lower-case hex helpers. */
public final class Hex {
    private static final HexFormat HEX = HexFormat.of();

    private Hex() {}

    public static String of(byte[] b) {
        return HEX.formatHex(b);
    }

    public static byte[] parse(String s) {
        return HEX.parseHex(s);
    }
}
