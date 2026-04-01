"""
sensor_client.py - Cliente Sensor (Python)
Simula 5 sensores IoT que envían mediciones al servidor central.

Sockets usados:
  - SOCK_STREAM (TCP) puerto 5000 → envío de mediciones y registro
  - SOCK_DGRAM  (UDP) puerto 5555 → health check PING/PONG

Uso: python sensor_client.py --host <servidor> [--port 5000] [--udp-port 5555]
"""

import socket
import threading
import time
import random
import argparse
import logging
import sys

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [%(name)s] %(levelname)s - %(message)s"
)


# ──────────────────────────────────────────────
# Definición de los 5 sensores simulados
# ──────────────────────────────────────────────
SENSOR_CONFIGS = [
    {
        "id":       "TEMPERATURA_1",
        "tipo":     "temperatura",
        "ubicacion": "Planta A",
        "unidad":   "C",
        "base":     10.0,
        "variacion": 85.0,
        "intervalo": 0.1,
    },
    {
        "id":       "VIBRACION_1",
        "tipo":     "vibracion",
        "ubicacion": "Linea 1",
        "unidad":   "mm/s",
        "base":     0.5,
        "variacion": 14.0,
        "intervalo": 0.1,
    },
    {
        "id":       "ENERGIA_1",
        "tipo":     "energia",
        "ubicacion": "Tablero Principal",
        "unidad":   "kWh",
        "base":     30.0,
        "variacion": 100.0,
        "intervalo": 0.1,
    },
    {
        "id":       "TEMPERATURA_2",
        "tipo":     "temperatura",
        "ubicacion": "Planta B",
        "unidad":   "C",
        "base":     10.0,
        "variacion": 85.0,
        "intervalo": 0.1,
    },
    {
        "id":       "PRESION_1",
        "tipo":     "presion",
        "ubicacion": "Sector 3",
        "unidad":   "bar",
        "base":     0.5,
        "variacion": 7.5,
        "intervalo": 0.1,
    },
]


# ──────────────────────────────────────────────
# Clase Sensor
# ──────────────────────────────────────────────
class Sensor:
    def __init__(self, config, server_host, server_tcp_port, server_udp_port):
        self.id          = config["id"]
        self.tipo        = config["tipo"]
        self.ubicacion   = config["ubicacion"]
        self.unidad      = config["unidad"]
        self.base        = config["base"]
        self.variacion   = config["variacion"]
        self.intervalo   = config["intervalo"]
        self.server_host = server_host
        self.tcp_port    = server_tcp_port
        self.udp_port    = server_udp_port
        self.logger      = logging.getLogger(self.id)
        self.running     = False
        self.tcp_sock    = None
        self.udp_sock    = None

    def _resolver_host(self, host, port, tipo_socket):
        """Resuelve el host usando getaddrinfo (sin hardcodear IPs)."""
        infos = socket.getaddrinfo(host, port, socket.AF_UNSPEC, tipo_socket)
        if not infos:
            raise ConnectionError(f"No se pudo resolver {host}")
        return infos[0]

    def _conectar_tcp(self):
        """Establece conexión TCP con el servidor."""
        af, socktype, proto, _, addr = self._resolver_host(
            self.server_host, self.tcp_port, socket.SOCK_STREAM)
        self.tcp_sock = socket.socket(af, socktype, proto)
        self.tcp_sock.settimeout(10)
        self.tcp_sock.connect(addr)
        self.logger.info(f"Conectado TCP a {self.server_host}:{self.tcp_port}")

    def _registrar(self):
        """Envía mensaje REGISTER al servidor."""
        msg = f"REGISTER|{self.id}|{self.tipo}|{self.ubicacion}\n"
        self.tcp_sock.sendall(msg.encode())
        resp = self.tcp_sock.recv(256).decode().strip()
        if resp.startswith("OK"):
            self.logger.info(f"Registro exitoso: {resp}")
            return True
        else:
            self.logger.error(f"Error en registro: {resp}")
            return False

    def _generar_valor(self):
        """Genera un valor simulado con posibilidad de anomalía."""
        return round(self.base + random.uniform(0, self.variacion), 2)

    def _enviar_medicion(self, valor):
        """Envía DATA al servidor y procesa respuesta."""
        ts  = int(time.time())
        msg = f"DATA|{self.id}|{valor}|{self.unidad}|{ts}\n"
        self.tcp_sock.sendall(msg.encode())
        resp = self.tcp_sock.recv(512).decode().strip()
        if resp.startswith("ALERT"):
            self.logger.warning(f"ALERTA recibida del servidor: {resp}")
        elif resp.startswith("OK"):
            self.logger.info(f"Medicion enviada: {valor} {self.unidad}")
        else:
            self.logger.error(f"Respuesta inesperada: {resp}")

    def _ping_loop(self):
        """Envía PING UDP al servidor cada 10 segundos."""
        af, socktype, proto, _, addr = self._resolver_host(
            self.server_host, self.udp_port, socket.SOCK_DGRAM)
        self.udp_sock = socket.socket(af, socktype, proto)
        self.udp_sock.settimeout(5)

        while self.running:
            try:
                msg = f"PING|{self.id}\n"
                self.udp_sock.sendto(msg.encode(), addr)
                data, _ = self.udp_sock.recvfrom(64)
                resp = data.decode().strip()
                if resp == "PONG":
                    self.logger.debug("PING → PONG OK")
            except socket.timeout:
                self.logger.warning("PING sin respuesta (timeout)")
            except Exception as e:
                self.logger.error(f"Error en PING: {e}")
            time.sleep(10)

        self.udp_sock.close()

    def _command_receiver(self):
        """Hilo que escucha comandos del servidor (PAUSE/RESUME)."""
        while self.running:
            try:
                if not self.tcp_sock:
                    time.sleep(1)
                    continue
                self.tcp_sock.settimeout(2)
                try:
                    data = self.tcp_sock.recv(256)
                    if data:
                        cmd = data.decode().strip()
                        if cmd == "PAUSE":
                            self.paused = True
                            self.logger.warning("Sensor PAUSADO por el servidor")
                        elif cmd == "RESUME":
                            self.paused = False
                            self.logger.info("Sensor REANUDADO por el servidor")
                except socket.timeout:
                    pass
            except Exception:
                time.sleep(1)

    def run(self):
        """Ciclo principal del sensor."""
        self.running = True
        self.paused  = False
        retry_delay  = 5

        # Hilos auxiliares
        ping_thread = threading.Thread(target=self._ping_loop, daemon=True)
        ping_thread.start()
        cmd_thread = threading.Thread(target=self._command_receiver, daemon=True)
        cmd_thread.start()

        while self.running:
            try:
                self._conectar_tcp()
                if not self._registrar():
                    self.logger.error("No se pudo registrar. Reintentando...")
                    time.sleep(retry_delay)
                    continue

                while self.running:
                    if not self.paused:
                        valor = self._generar_valor()
                        self._enviar_medicion(valor)
                    else:
                        self.logger.debug("Pausado, esperando...")
                    time.sleep(self.intervalo)

            except (ConnectionRefusedError, OSError) as e:
                self.logger.error(f"Error de conexion: {e}. Reintentando en {retry_delay}s")
                time.sleep(retry_delay)
            except Exception as e:
                self.logger.error(f"Error inesperado: {e}")
                time.sleep(retry_delay)
            finally:
                if self.tcp_sock:
                    try:
                        self.tcp_sock.close()
                    except Exception:
                        pass
                    self.tcp_sock = None

    def stop(self):
        self.running = False


# ──────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Cliente Sensor IoT")
    parser.add_argument("--host",     required=True, help="Host/dominio del servidor")
    parser.add_argument("--port",     type=int, default=5000, help="Puerto TCP del servidor")
    parser.add_argument("--udp-port", type=int, default=5555, help="Puerto UDP health check")
    args = parser.parse_args()

    logging.info(f"Iniciando {len(SENSOR_CONFIGS)} sensores → {args.host}:{args.port}")

    sensores = []
    hilos    = []

    for cfg in SENSOR_CONFIGS:
        s = Sensor(cfg, args.host, args.port, args.udp_port)
        sensores.append(s)
        t = threading.Thread(target=s.run, daemon=True, name=cfg["id"])
        hilos.append(t)
        t.start()
        time.sleep(0.5)  # escalonar conexiones

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        logging.info("Deteniendo sensores...")
        for s in sensores:
            s.stop()
        sys.exit(0)


if __name__ == "__main__":
    main()
