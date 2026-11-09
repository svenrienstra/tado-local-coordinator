package tadocoord;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

/** golden/vectors.txt: frames produced by freedo's builders (tools/golden). */
public final class Golden {
    private static final Map<String, byte[]> VECTORS = load();

    private Golden() {}

    public static byte[] get(String name) {
        byte[] v = VECTORS.get(name);
        if (v == null) {
            throw new IllegalArgumentException("no golden vector " + name);
        }
        return v.clone();
    }

    public static Eui eui(String name) {
        return new Eui(get(name));
    }

    private static Map<String, byte[]> load() {
        Path path = Path.of(System.getProperty("golden.vectors", "../golden/vectors.txt"));
        Map<String, byte[]> m = new HashMap<>();
        try {
            for (String line : Files.readAllLines(path)) {
                if (line.isBlank() || line.startsWith("#")) {
                    continue;
                }
                String[] f = line.trim().split("\\s+");
                m.put(f[0], Hex.parse(f[1]));
            }
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        return m;
    }
}
