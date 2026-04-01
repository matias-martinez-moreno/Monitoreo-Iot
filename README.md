# Sistema de Monitoreo de Sensores IoT

**Autor:** Matías Martínez Moreno

Sistema distribuido que monitorea 7 sensores IoT en tiempo real. Un servidor central (C con sockets Berkeley) recibe mediciones, detecta anomalías y notifica operadores vía web dashboard.

## Componentes

- **Servidor** (`server/server.c`): TCP sensores (5000), operadores (5001), HTTP web (8080), UDP health check (5555)
- **Sensores Python** (5): TEMPERATURA_1, TEMPERATURA_2, VIBRACION_1, ENERGIA_1, PRESION_1
- **Sensores Java** (2): PRESION_2, HUMEDAD_1
- **Autenticación** (`auth/auth_service.py`): Flask en puerto 9000
- **Web** (`web/`): Dashboard con gráficas, alertas, umbrales, CSV

## Ejecución local

```bash
# Servidor
./server 5000 server.log

# Autenticación
python3 auth/auth_service.py --host 0.0.0.0 --port 9000

# Sensores Python
python3 clients/python/sensor_client.py --host <servidor>

# Sensores Java
cd clients/java && javac SensorClient.java
java SensorClient --host <servidor>
```

**Web:** `http://<servidor>:8080`  
**Credenciales:** admin/admin123, operador/oper2024, ingeniero/ing2024

## Despliegue en AWS

### Requisitos
- Instancia EC2 (Ubuntu 20.04+)
- Docker instalado
- Puertos abiertos: 5000, 5001, 8080 (TCP), 5555 (UDP)

### Pasos

**1. Clonar y compilar**
```bash
cd ~
git clone https://github.com/matias-martinez-moreno/Monitoreo-Iot.git
cd Monitoreo-Iot
```

**2. Compilar imagen Docker**
```bash
sudo docker build -t iot-server .
```

**3. Obtener IP interna de EC2**
```bash
hostname -I
# Anotarla (ej: 172.31.18.30)
```

**4. Iniciar servidor** (Terminal 1)
```bash
sudo docker run -d \
  -p 5000:5000 -p 5001:5001 -p 8080:8080 -p 5555:5555/udp \
  -e AUTH_HOST=<IP-INTERNA> \
  -e AUTH_PORT=9000 \
  --name iot-server \
  iot-server
```

**5. Iniciar autenticación** (Terminal 2)
```bash
python3 auth/auth_service.py --host 0.0.0.0 --port 9000
```

**6. Iniciar sensores Python** (Terminal 3)
```bash
python3 clients/python/sensor_client.py --host <IP-PUBLICA>
```

**7. Iniciar sensores Java** (Terminal 4)
```bash
cd clients/java
javac -source 8 -target 8 SensorClient.java
java SensorClient --host <IP-PUBLICA>
```

### Acceso
- **Web:** `http://<IP-PUBLICA>:8080`
- **Login:** admin/admin123, operador/oper2024, ingeniero/ing2024

### Detener el sistema
```bash
sudo docker stop iot-server
sudo docker rm iot-server
pkill -f auth_service
pkill -f sensor_client
pkill -f SensorClient
```

## DNS y resolución de nombres

El sistema no tiene direcciones IP hardcodeadas en el código. Todos los servicios usan resolución de nombres:
- **C**: `getaddrinfo()` (server.c línea 202)
- **Python**: `socket.getaddrinfo()` (sensor_client.py línea 99)
- **Java**: `InetAddress.getByName()` (SensorClient.java línea 63)

**Dirección:** IP pública asignada por AWS EC2 (varía según despliegue)

**Nota sobre Route 53:** Se intentó implementar DNS con Route 53, pero las restricciones de permisos en la cuenta de AWS del laboratorio (IAM role sin `route53domains` full access) lo impidieron. El código está preparado para funcionar con cualquier dominio usando `getaddrinfo()`, por lo que Route 53 puede ser configurado en una cuenta con permisos administrativos.

## Documentación

- `PROTOCOLO.md`: especificación completa del protocolo TCP/UDP/HTTP
- `Dockerfile`: despliegue en contenedor
