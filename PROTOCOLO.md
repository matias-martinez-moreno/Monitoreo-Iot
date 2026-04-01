# Especificación del Protocolo de Aplicación

## Introducción

Este protocolo define la comunicación entre sensores IoT, un servidor central de monitoreo y operadores en una red distribuida. La comunicación se realiza mediante tres canales independientes: TCP para sensores (5000), TCP para operadores (5001), UDP para health check (5555), y HTTP para la interfaz web (8080).

---

## 1. Canales de Comunicación

### TCP Puerto 5000 — Sensores
Los dispositivos sensores se conectan al servidor para registrarse y enviar mediciones periódicas. El servidor procesa las mediciones, las almacena y detecta anomalías.

### TCP Puerto 5001 — Operadores
Los operadores (humanos u otros sistemas) reciben notificaciones push cuando el servidor detecta anomalías. También pueden consultar el estado general del sistema.

### UDP Puerto 5555 — Health Check
Los sensores envían paquetes PING periódicamente para confirmar que el servidor está disponible. Sin respuesta PONG en 3 intentos consecutivos, el sensor se marca como desconectado.

### TCP Puerto 8080 — HTTP Web
Interfaz web para autenticación, visualización del dashboard, consulta de sensores activos y modificación de umbrales de alerta.

---

## 2. Protocolo de Mensajes TCP

Todos los mensajes TCP tienen el formato:
```
COMANDO|campo1|campo2|...\n
```

Separador: `|` (pipe)  
Terminador: `\n` (newline)  
Tamaño máximo: 2048 bytes

---

## 3. Operaciones — Sensores

### 3.1 REGISTER

El sensor informa al servidor de su existencia y tipo. Debe ejecutarse una sola vez al conectarse.

**Formato:**
```
REGISTER|sensor_id|tipo|ubicacion
```

**Campos:**
- `sensor_id`: identificador único (ej: TEMPERATURA_1)
- `tipo`: categoria del sensor (temperatura, vibracion, energia, presion, humedad)
- `ubicacion`: localizacion fisica (ej: Planta A, Sector 2)

**Respuestas del servidor:**
```
OK|sensor_id                    registro exitoso
ERROR|motivo                    error en el registro
```

**Ejemplo:**
```
→ REGISTER|TEMPERATURA_1|temperatura|Planta A
← OK|TEMPERATURA_1
```

---

### 3.2 DATA

El sensor envía una medición. Se ejecuta periódicamente (intervalo configurable, en este caso 0.1 segundos).

**Formato:**
```
DATA|sensor_id|valor|unidad|timestamp
```

**Campos:**
- `sensor_id`: id del sensor registrado
- `valor`: medición numérica (ej: 45.3)
- `unidad`: unidad de medida (C, mm/s, kWh, bar, %)
- `timestamp`: timestamp unix del momento de medición

**Respuestas del servidor:**
```
OK                              medición aceptada, sin anomalia
ALERT|descripcion               medición aceptada, valor supera umbral
ERROR|motivo                    error en el mensaje
```

Si se detecta anomalía, el servidor además notifica a todos los operadores conectados.

**Ejemplo normal:**
```
→ DATA|TEMPERATURA_1|54.3|C|1711900800
← OK
```

**Ejemplo con anomalía:**
```
→ DATA|TEMPERATURA_1|92.5|C|1711900810
← ALERT|TEMP_ALTA:92.50 C supera limite de 80 C
```

---

### 3.3 STATUS

Cualquier cliente conectado solicita el estado actual de todos los sensores.

**Formato:**
```
STATUS
```

**Respuesta del servidor:**
```
SENSORS|id:tipo:valor:unidad:estado|id:tipo:valor:unidad:estado|...
```

Donde `estado` es: ONLINE, OFFLINE, o PAUSED.

**Ejemplo:**
```
→ STATUS
← SENSORS|TEMPERATURA_1:temperatura:54.3:C:ONLINE|PRESION_2:presion:4.1:bar:ONLINE|HUMEDAD_1:humedad:58.4:%:ONLINE
```

---

## 4. Operaciones — Operadores

### 4.1 NOTIFY

El servidor envía este mensaje a los operadores cuando se detecta una anomalía. No requiere respuesta.

**Formato:**
```
NOTIFY|sensor_id|tipo_alerta|valor|timestamp
```

**Campos:**
- `sensor_id`: sensor que disparó la alerta
- `tipo_alerta`: descripción de la anomalía
- `valor`: valor medido que causó la alerta
- `timestamp`: momento de la anomalía

**Ejemplos:**
```
← NOTIFY|TEMPERATURA_1|TEMP_ALTA:93.1 C supera limite de 80 C|93.1|1711900810
← NOTIFY|VIBRACION_1|VIBRACION_CRITICA:14.5 mm/s supera limite|14.5|1711900815
← NOTIFY|ENERGIA_1|CONSUMO_EXCESIVO:120.0 kWh supera limite|120.0|1711900820
← NOTIFY|TEMPERATURA_1|SENSOR_OFFLINE|0|1711900900
```

---

## 5. Health Check UDP

### PING / PONG

**Formato:**
```
PING|sensor_id
```

**Respuesta:**
```
PONG
```

El sensor envía PING cada 10 segundos. Si no recibe PONG después de 3 intentos, registra fallo de conectividad.

**Ejemplo:**
```
→ PING|TEMPERATURA_1
← PONG
```

---

## 6. Protocolo HTTP (Puerto 8080)

| Método | Ruta | Descripción | Respuesta |
|--------|------|-------------|-----------|
| GET | / | Página de login | HTML |
| GET | /login?user=X&pass=Y | Autenticación | HTML dashboard o redirección |
| GET | /dashboard | Panel de operador | HTML |
| GET | /sensors | Estado de sensores | JSON |
| GET | /threshold?type=X&value=Y | Cambiar umbral | JSON confirmación |
| GET | /pause?sensor=X | Pausar sensor | JSON confirmación |
| GET | /resume?sensor=X | Reanudar sensor | JSON confirmación |

**Códigos HTTP usados:**
- 200 OK: solicitud exitosa
- 302 Found: redirección (login incorrecto)
- 404 Not Found: ruta no existe
- 405 Method Not Allowed: método no soportado (solo GET)

---

## 7. Umbrales de Alerta

Cada tipo de sensor tiene un umbral configurable en tiempo real desde el dashboard.

| Tipo | Unidad | Umbral por defecto | Alerta generada |
|------|--------|-----------|-----------|
| temperatura | °C | 80 | TEMP_ALTA |
| vibracion | mm/s | 10 | VIBRACION_CRITICA |
| energia | kWh | 100 | CONSUMO_EXCESIVO |
| presion | bar | 5 | PRESION_ALTA |
| humedad | % | 80 | HUMEDAD_ALTA |

---

## 8. Manejo de Errores

El servidor responde con ERROR ante:

| Situación | Respuesta |
|-----------|-----------|
| Formato de mensaje invalido | ERROR\|Formato invalido |
| Sensor no registrado envia DATA | ERROR\|Sensor no registrado |
| Capacidad maxima alcanzada | ERROR\|Servidor lleno |
| Comando no reconocido | ERROR\|Comando desconocido |
| Credenciales invalidas (web) | HTTP 302 → /login?error=1 |

---

## 9. Flujo Completo de Operación

```
1. Sensor se conecta (TCP :5000) y se registra
   → REGISTER|PRESION_2|presion|Sector 2
   ← OK|PRESION_2

2. Sensor envía mediciones periodicamente (cada 0.1 s)
   → DATA|PRESION_2|3.40|bar|1711900800
   ← OK

3. Sensor envia PING (cada 10 s, UDP :5555)
   → PING|PRESION_2
   ← PONG

4. Medicion supera umbral
   → DATA|PRESION_2|6.80|bar|1711900810
   ← ALERT|PRESION_ALTA:6.80 bar supera limite de 5 bar

   Servidor notifica a operadores en :5001:
   ← NOTIFY|PRESION_2|PRESION_ALTA:6.80 bar supera limite|6.80|1711900810

5. Operador consulta estado general
   → STATUS
   ← SENSORS|PRESION_2:presion:6.8:bar:ONLINE|...

6. Usuario accede a dashboard web
   GET /login?user=admin&pass=admin123 HTTP/1.1
   ← HTTP/1.1 200 OK + HTML dashboard

7. Usuario modifica umbral desde web
   GET /threshold?type=presion&value=6.0 HTTP/1.1
   ← HTTP/1.1 200 OK + {"status":"ok"}
```

---

## 10. Implementación: Resolución de Nombres

El código no tiene direcciones IP hardcodeadas. Se usa:
- **C**: `getaddrinfo()` en server.c (línea 202) para conectar al servicio de autenticación
- **Python**: `socket.getaddrinfo()` en sensor_client.py (línea 99)
- **Java**: `InetAddress.getByName()` en SensorClient.java (línea 63)

Todos los clientes y servicios aceptan el host como parámetro de línea de comandos (`--host`), permitiendo usar dominios o direcciones IP.
