package com.aman.aqm;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;

/**
 * Minimal MQTT 3.1.1 subscriber (no library): CONNECT with username/password,
 * one SUBSCRIBE (QoS 1), receives PUBLISH QoS 0/1 (PUBACKs QoS 1), PINGREQ
 * keep-alive. Tries each host in order, reconnects with 1..10 s back-off.
 * All socket I/O happens on this one thread.
 */
final class Mqtt implements Runnable {

    interface Listener {
        /** state: "connecting", "connected", "error: ..."; host + remote IP when known */
        void onState(String state, String host, String remoteIp);
        void onMessage(String topic, String payload);
    }

    private static final int KEEPALIVE_S = 30;

    private final String[] hosts;
    private final int port;
    private final String user, pass, filter;
    private final Listener l;
    private volatile boolean running = true;
    private volatile Socket sock;

    Mqtt(String[] hosts, int port, String user, String pass, String filter, Listener l) {
        this.hosts = hosts;
        this.port = port;
        this.user = user;
        this.pass = pass;
        this.filter = filter;
        this.l = l;
    }

    void stop() {
        running = false;
        closeQuiet();
    }

    @Override
    public void run() {
        int backoff = 1000;
        while (running) {
            for (String h : hosts) {
                h = h.trim();
                if (!running || h.isEmpty())
                    continue;
                l.onState("connecting", h, null);
                try {
                    session(h);
                    backoff = 1000;
                } catch (Exception e) {
                    if (running)
                        l.onState("error: " + (e.getMessage() != null ? e.getMessage()
                                : e.getClass().getSimpleName()), h, null);
                }
                closeQuiet();
            }
            try {
                Thread.sleep(backoff);
            } catch (InterruptedException e) {
                return;
            }
            backoff = Math.min(backoff * 2, 10000);
        }
    }

    private void session(String host) throws IOException {
        Socket s = new Socket();
        sock = s;
        s.connect(new InetSocketAddress(host, port), 3000);
        s.setTcpNoDelay(true);
        InputStream in = new BufferedInputStream(s.getInputStream());
        OutputStream out = s.getOutputStream();

        /* CONNECT */
        ByteArrayOutputStream v = new ByteArrayOutputStream();
        str(v, "MQTT");
        v.write(4);                                 /* protocol level 3.1.1 */
        int flags = 0x02;                           /* clean session */
        boolean hasUser = user != null && !user.isEmpty();
        boolean hasPass = hasUser && pass != null && !pass.isEmpty();
        if (hasUser) flags |= 0x80;
        if (hasPass) flags |= 0x40;
        v.write(flags);
        v.write(KEEPALIVE_S >> 8);
        v.write(KEEPALIVE_S & 0xff);
        str(v, "aqm-app-" + Long.toHexString(System.nanoTime() & 0xffffffL));
        if (hasUser) str(v, user);
        if (hasPass) str(v, pass);
        send(out, 0x10, v.toByteArray());

        /* CONNACK */
        s.setSoTimeout(5000);
        int[] hdr = new int[1];
        byte[] body = readPacket(in, hdr, true);
        if ((hdr[0] >> 4) != 2 || body.length < 2)
            throw new IOException("no CONNACK");
        if (body[1] != 0)
            throw new IOException(connackText(body[1]));

        /* SUBSCRIBE id 1, QoS 1 */
        v = new ByteArrayOutputStream();
        v.write(0);
        v.write(1);
        str(v, filter);
        v.write(1);
        send(out, 0x82, v.toByteArray());

        l.onState("connected", host, s.getInetAddress().getHostAddress());

        long lastTx = System.currentTimeMillis(), lastRx = lastTx;
        s.setSoTimeout(1000);
        while (running) {
            body = readPacket(in, hdr, false);
            long now = System.currentTimeMillis();
            if (body != null) {
                lastRx = now;
                int type = hdr[0] >> 4;
                if (type == 3) {
                    handlePublish(hdr[0], body, out);
                } else if (type == 9) {             /* SUBACK */
                    if (body.length >= 3 && (body[2] & 0xff) == 0x80)
                        throw new IOException("subscribe refused (ACL?)");
                }
            }
            if (now - lastTx > (KEEPALIVE_S * 1000L) / 2) {
                send(out, 0xC0, new byte[0]);       /* PINGREQ */
                lastTx = now;
            }
            if (now - lastRx > KEEPALIVE_S * 2000L)
                throw new IOException("broker silent");
        }
    }

    private void handlePublish(int h, byte[] b, OutputStream out) throws IOException {
        int qos = (h >> 1) & 3;
        int tl = ((b[0] & 0xff) << 8) | (b[1] & 0xff);
        String topic = new String(b, 2, tl, StandardCharsets.UTF_8);
        int p = 2 + tl;
        if (qos > 0) {
            byte[] ack = { b[p], b[p + 1] };
            p += 2;
            send(out, 0x40, ack);                   /* PUBACK */
        }
        l.onMessage(topic, new String(b, p, b.length - p, StandardCharsets.UTF_8));
    }

    /** Returns null on idle timeout (only before the first byte of a packet). */
    private byte[] readPacket(InputStream in, int[] hdr, boolean mustArrive) throws IOException {
        int first;
        try {
            first = in.read();
        } catch (SocketTimeoutException e) {
            if (mustArrive)
                throw new IOException("broker did not answer");
            return null;
        }
        if (first < 0)
            throw new EOFException("connection closed by broker");
        hdr[0] = first;
        Socket s = sock;
        int old = s.getSoTimeout();
        s.setSoTimeout(10000);                      /* rest of the packet must follow */
        int len = 0, mul = 1, c;
        do {
            c = in.read();
            if (c < 0) throw new EOFException("connection closed");
            len += (c & 127) * mul;
            mul *= 128;
        } while ((c & 128) != 0 && mul <= 128 * 128 * 128);
        byte[] b = new byte[len];
        int got = 0;
        while (got < len) {
            int r = in.read(b, got, len - got);
            if (r < 0) throw new EOFException("connection closed");
            got += r;
        }
        s.setSoTimeout(old);
        return b;
    }

    private static void send(OutputStream out, int h, byte[] body) throws IOException {
        ByteArrayOutputStream p = new ByteArrayOutputStream();
        p.write(h);
        int len = body.length;
        do {
            int d = len % 128;
            len /= 128;
            if (len > 0) d |= 128;
            p.write(d);
        } while (len > 0);
        p.write(body, 0, body.length);
        out.write(p.toByteArray());
        out.flush();
    }

    private static void str(ByteArrayOutputStream v, String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        v.write(b.length >> 8);
        v.write(b.length & 0xff);
        v.write(b, 0, b.length);
    }

    private static String connackText(int rc) {
        switch (rc) {
            case 4: return "wrong username or password";
            case 5: return "not authorised (check username / password)";
            case 3: return "broker unavailable";
            default: return "connection refused (" + rc + ")";
        }
    }

    private void closeQuiet() {
        Socket s = sock;
        if (s != null) {
            try { s.close(); } catch (IOException ignored) { }
        }
    }
}
