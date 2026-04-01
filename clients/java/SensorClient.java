/**
 * SensorClient.java - Cliente Sensor (Java)
 * Segundo lenguaje de cliente — requerido por el proyecto.
 *
 * Simula 2 sensores adicionales:
 *   - PRESSURE_JAVA_001 : presión hidráulica (bar)
 *   - HUMIDITY_001      : humedad ambiental  (%)
 *
 * Sockets usados:
 *   - java.net.Socket       → SOCK_STREAM / TCP port 5000 (mediciones)
 *   - java.net.DatagramSocket → SOCK_DGRAM / UDP port 5555 (health check)
 *
 * Uso: java SensorClient --host <servidor> [--port 5000] [--udp-port 5555]
 */

import java.io.*;
import java.net.*;
import java.time.Instant;
import java.util.Random;
import java.util.logging.*;

public class SensorClient {

    private static final Logger LOGGER = Logger.getLogger(SensorClient.class.getName());

    // ── Configuración de sensor ───────────────────────────
    static class SensorConfig {
        String id, tipo, ubicacion, unidad;
        double base, variacion, intervalo;

        SensorConfig(String id, String tipo, String ubicacion,
                     String unidad, double base, double variacion, double intervalo) {
            this.id        = id;   this.tipo      = tipo;
            this.ubicacion = ubicacion; this.unidad    = unidad;
            this.base      = base; this.variacion = variacion;
            this.intervalo = intervalo;
        }
    }

    // ── Hilo de sensor ────────────────────────────────────
    static class SensorThread extends Thread {
        private final SensorConfig cfg;
        private final String       host;
        private final int          tcpPort, udpPort;
        private volatile boolean   running = true, paused = false;
        private final Random       rnd     = new Random();

        SensorThread(SensorConfig cfg, String host, int tcpPort, int udpPort) {
            this.cfg = cfg; this.host = host;
            this.tcpPort = tcpPort; this.udpPort = udpPort;
            setName(cfg.id); setDaemon(true);
        }

        @Override
        public void run() {
            Thread pingThread = new Thread(this::pingLoop, cfg.id + "-UDP");
            pingThread.setDaemon(true);
            pingThread.start();

            while (running) {
                try {
                    InetAddress addr = InetAddress.getByName(host);
                    try (Socket sock = new Socket()) {
                        sock.connect(new InetSocketAddress(addr, tcpPort), 10000);

                        BufferedReader in  = new BufferedReader(new InputStreamReader(sock.getInputStream()));
                        PrintWriter    out = new PrintWriter(new OutputStreamWriter(sock.getOutputStream()), true);

                        // Hilo receptor de comandos (PAUSE/RESUME)
                        Thread cmdReceiver = new Thread(() -> {
                            try {
                                String line;
                                while (running && (line = in.readLine()) != null) {
                                    if ("PAUSE".equals(line.trim())) {
                                        paused = true;
                                        LOGGER.warning("[" + cfg.id + "] Sensor PAUSADO por el servidor");
                                    } else if ("RESUME".equals(line.trim())) {
                                        paused = false;
                                        LOGGER.info("[" + cfg.id + "] Sensor REANUDADO por el servidor");
                                    } else if (line.startsWith("ALERT")) {
                                        LOGGER.warning("[" + cfg.id + "] ALERTA: " + line);
                                    }
                                }
                            } catch (IOException ignored) {}
                        }, cfg.id + "-CMD");
                        cmdReceiver.setDaemon(true);
                        cmdReceiver.start();

                        // REGISTER
                        String reg = String.format("REGISTER|%s|%s|%s", cfg.id, cfg.tipo, cfg.ubicacion);
                        out.println(reg);
                        LOGGER.info("[" + cfg.id + "] → " + reg);

                        Thread.sleep(500); // esperar respuesta en receptor

                        // Ciclo de envío
                        while (running && sock.isConnected()) {
                            if (!paused) {
                                double val = Math.round((cfg.base + rnd.nextDouble() * cfg.variacion) * 100.0) / 100.0;
                                long   ts  = Instant.now().getEpochSecond();
                                String data = String.format("DATA|%s|%.2f|%s|%d", cfg.id, val, cfg.unidad, ts);
                                out.println(data);
                                LOGGER.info("[" + cfg.id + "] Medicion: " + val + " " + cfg.unidad);
                            } else {
                                LOGGER.fine("[" + cfg.id + "] Pausado...");
                            }
                            Thread.sleep((long)(cfg.intervalo * 1000));
                        }
                    }
                } catch (IOException e) {
                    LOGGER.severe("[" + cfg.id + "] Conexion perdida: " + e.getMessage() + " — reintentando en 5s");
                    try { Thread.sleep(5000); } catch (InterruptedException ie) { break; }
                } catch (InterruptedException e) { break; }
            }
        }

        // PING UDP cada 10 segundos
        private void pingLoop() {
            try (DatagramSocket udpSock = new DatagramSocket()) {
                udpSock.setSoTimeout(5000);
                InetAddress addr = InetAddress.getByName(host);
                while (running) {
                    try {
                        byte[] msg = ("PING|" + cfg.id + "\n").getBytes();
                        udpSock.send(new DatagramPacket(msg, msg.length, addr, udpPort));
                        byte[]         buf  = new byte[64];
                        DatagramPacket resp = new DatagramPacket(buf, buf.length);
                        udpSock.receive(resp);
                        String pong = new String(resp.getData(), 0, resp.getLength()).trim();
                        if ("PONG".equals(pong)) LOGGER.fine("[" + cfg.id + "] PING → PONG OK");
                    } catch (SocketTimeoutException e) {
                        LOGGER.warning("[" + cfg.id + "] PING timeout");
                    }
                    Thread.sleep(10000);
                }
            } catch (Exception e) {
                LOGGER.severe("[" + cfg.id + "] Error UDP: " + e.getMessage());
            }
        }

        void stopSensor() { running = false; interrupt(); }
    }

    // ── Main ──────────────────────────────────────────────
    public static void main(String[] args) throws InterruptedException {
        // Logger
        Logger root = Logger.getLogger("");
        root.setLevel(Level.INFO);
        for (Handler h : root.getHandlers())
            h.setFormatter(new SimpleFormatter() {
                @Override public synchronized String format(LogRecord lr) {
                    return String.format("[%1$tF %1$tT] %2$s%n",
                        new java.util.Date(lr.getMillis()), lr.getMessage());
                }
            });

        // Parámetros
        String host    = "localhost";
        int    port    = 5000;
        int    udpPort = 5555;

        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--host":     if (i+1 < args.length) host    = args[++i]; break;
                case "--port":     if (i+1 < args.length) port    = Integer.parseInt(args[++i]); break;
                case "--udp-port": if (i+1 < args.length) udpPort = Integer.parseInt(args[++i]); break;
            }
        }

        LOGGER.info("Cliente Sensor Java iniciado → " + host + ":" + port);

        // 2 sensores adicionales (complementan los 5 del cliente Python)
        SensorConfig[] configs = {
            new SensorConfig("PRESION_2", "presion", "Sector 2", "bar", 0.5,  7.5,  0.1),
            new SensorConfig("HUMEDAD_1",  "humedad", "Planta A", "%",   20.0, 75.0, 0.1),
        };

        SensorThread[] threads = new SensorThread[configs.length];
        for (int i = 0; i < configs.length; i++) {
            threads[i] = new SensorThread(configs[i], host, port, udpPort);
            threads[i].start();
            Thread.sleep(600);
        }

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            LOGGER.info("Deteniendo sensores Java...");
            for (SensorThread t : threads) t.stopSensor();
        }));

        for (SensorThread t : threads) t.join();
    }
}
