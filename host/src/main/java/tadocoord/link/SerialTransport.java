package tadocoord.link;

import com.fazecast.jSerialComm.SerialPort;
import java.io.IOException;

/** The host link over a USB serial port (the CC1312 board's USB-serial chip). */
public final class SerialTransport implements HostLink.Transport {
    static final int BAUD = 460_800;

    private final SerialPort port;

    public SerialTransport(String device) throws IOException {
        port = SerialPort.getCommPort(device);
        port.setComPortParameters(BAUD, 8, SerialPort.ONE_STOP_BIT, SerialPort.NO_PARITY);
        port.setFlowControl(SerialPort.FLOW_CONTROL_DISABLED);
        // Semi-blocking: a read returns as soon as at least one byte is there.
        port.setComPortTimeouts(SerialPort.TIMEOUT_READ_SEMI_BLOCKING, 0, 0);
        if (!port.openPort()) {
            throw new IOException("cannot open " + device);
        }
    }

    @Override
    public void write(byte[] b) throws IOException {
        if (port.writeBytes(b, b.length) != b.length) {
            throw new IOException("short write to " + port.getSystemPortName());
        }
    }

    @Override
    public int read(byte[] buf) throws IOException {
        int n = port.readBytes(buf, buf.length);
        if (n < 0) {
            throw new IOException("read failed on " + port.getSystemPortName());
        }
        return n;
    }

    @Override
    public void close() {
        port.closePort();
    }
}
