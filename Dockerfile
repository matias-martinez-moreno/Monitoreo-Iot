# Dockerfile — Servidor Central IoT
# Compila server.c con gcc y lo ejecuta en el contenedor

FROM gcc:13

# Instalar dependencias del sistema
RUN apt-get update && apt-get install -y \
    make \
    && rm -rf /var/lib/apt/lists/*

# Directorio de trabajo
WORKDIR /app

# Copiar fuente del servidor
COPY server/ ./server/
COPY web/     ./web/

# Crear directorio de logs
RUN mkdir -p logs

# Compilar el servidor
WORKDIR /app/server
RUN make

# Volver al directorio principal
WORKDIR /app

# Puertos expuestos:
#   5000/tcp → sensores
#   5001/tcp → operadores
#   8080/tcp → interfaz web HTTP
#   5555/udp → health check PING/PONG
EXPOSE 5000/tcp
EXPOSE 5001/tcp
EXPOSE 8080/tcp
EXPOSE 5555/udp

# Variables de entorno para el servicio de autenticación
# Sobreescribir con: docker run -e AUTH_HOST=mi.dominio.com ...
ENV AUTH_HOST=localhost
ENV AUTH_PORT=9000

# Comando de inicio: ./server puerto archivoDeLogs
CMD ["./server/server", "5000", "logs/server.log"]
