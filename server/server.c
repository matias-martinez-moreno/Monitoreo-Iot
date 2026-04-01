/*
 * server.c - Servidor Central de Monitoreo IoT
 *
 * Sockets Berkeley:
 *   - SOCK_STREAM (TCP) puerto 5000: sensores
 *   - SOCK_STREAM (TCP) puerto 5001: operadores
 *   - SOCK_STREAM (TCP) puerto 8080: HTTP web
 *   - SOCK_DGRAM  (UDP) puerto 5555: health check PING/PONG
 *
 * Uso: ./server puerto archivoDeLogs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

/* ── Puertos ──────────────────────────────────────────── */
#define PORT_SENSORS   5000
#define PORT_OPERATORS 5001
#define PORT_HTTP      8080
#define PORT_UDP       5555

/* ── Límites ──────────────────────────────────────────── */
#define MAX_SENSORS   50
#define MAX_OPERATORS 20
#define MAX_BUF       2048

/* ── Umbrales de alerta (modificables en tiempo real) ─── */
static double THRESH_TEMP     = 80.0;
static double THRESH_VIB      = 10.0;
static double THRESH_ENERGY   = 100.0;
static double THRESH_PRESSURE = 5.0;
static pthread_mutex_t thresh_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Estructuras ──────────────────────────────────────── */
typedef enum { STATUS_ONLINE, STATUS_OFFLINE, STATUS_PAUSED } SensorStatus;

typedef struct {
    char   id[64];
    char   type[32];
    char   location[64];
    double last_value;
    char   unit[16];
    time_t last_seen;
    SensorStatus status;
    int    tcp_fd;
    int    ping_miss;
} Sensor;

typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
    int  active;
} Operator;

/* ── Estado global ────────────────────────────────────── */
static Sensor   sensors[MAX_SENSORS];
static int      sensor_count = 0;
static pthread_mutex_t sensors_mutex = PTHREAD_MUTEX_INITIALIZER;

static Operator operators[MAX_OPERATORS];
static int      operator_count = 0;
static pthread_mutex_t operators_mutex = PTHREAD_MUTEX_INITIALIZER;

static FILE   *log_fp  = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static char auth_host[256] = "localhost";
static int  auth_port      = 9000;

/* ═══════════════════════════════════════════════════════
 *  LOGGING
 * ═══════════════════════════════════════════════════════ */
static void log_entry(const char *client_ip, int client_port,
                      const char *msg, const char *response)
{
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

    pthread_mutex_lock(&log_mutex);
    fprintf(stdout, "[%s] IP:%-15s PORT:%-6d MSG:%-40s RESP:%s\n",
            ts,
            client_ip  ? client_ip  : "N/A",
            client_port,
            msg        ? msg        : "N/A",
            response   ? response   : "N/A");
    if (log_fp) {
        fprintf(log_fp, "[%s] IP:%-15s PORT:%-6d MSG:%-40s RESP:%s\n",
                ts,
                client_ip  ? client_ip  : "N/A",
                client_port,
                msg        ? msg        : "N/A",
                response   ? response   : "N/A");
        fflush(log_fp);
    }
    pthread_mutex_unlock(&log_mutex);
}

static void log_error_msg(const char *msg)
{
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

    pthread_mutex_lock(&log_mutex);
    fprintf(stderr, "[%s] ERROR: %s\n", ts, msg);
    if (log_fp) {
        fprintf(log_fp, "[%s] ERROR: %s\n", ts, msg);
        fflush(log_fp);
    }
    pthread_mutex_unlock(&log_mutex);
}

/* ═══════════════════════════════════════════════════════
 *  UTILIDADES DE SENSORES
 * ═══════════════════════════════════════════════════════ */
static Sensor *find_sensor(const char *id)
{
    for (int i = 0; i < sensor_count; i++)
        if (strcmp(sensors[i].id, id) == 0) return &sensors[i];
    return NULL;
}

/* Enviar comando a un sensor por su fd */
static void send_to_sensor(const char *sensor_id, const char *msg)
{
    pthread_mutex_lock(&sensors_mutex);
    for (int i = 0; i < sensor_count; i++) {
        if (strcmp(sensors[i].id, sensor_id) == 0 && sensors[i].tcp_fd > 0) {
            send(sensors[i].tcp_fd, msg, strlen(msg), MSG_NOSIGNAL);
            break;
        }
    }
    pthread_mutex_unlock(&sensors_mutex);
}

/* Retorna descripción de alerta o NULL si no hay anomalía */
static const char *check_alert(const char *type, double value)
{
    static char alert[256];
    pthread_mutex_lock(&thresh_mutex);
    double tt = THRESH_TEMP, tv = THRESH_VIB, te = THRESH_ENERGY, tp = THRESH_PRESSURE;
    pthread_mutex_unlock(&thresh_mutex);

    if (strcmp(type, "temperatura") == 0 && value > tt) {
        snprintf(alert, sizeof(alert), "TEMP_ALTA:%.2f C supera limite de %.0f C", value, tt);
        return alert;
    }
    if (strcmp(type, "vibracion") == 0 && value > tv) {
        snprintf(alert, sizeof(alert), "VIBRACION_CRITICA:%.2f mm/s supera limite de %.0f mm/s", value, tv);
        return alert;
    }
    if (strcmp(type, "energia") == 0 && value > te) {
        snprintf(alert, sizeof(alert), "CONSUMO_EXCESIVO:%.2f kWh supera limite de %.0f kWh", value, te);
        return alert;
    }
    if (strcmp(type, "presion") == 0 && value > tp) {
        snprintf(alert, sizeof(alert), "PRESION_ALTA:%.2f bar supera limite de %.0f bar", value, tp);
        return alert;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  NOTIFICACION A OPERADORES
 * ═══════════════════════════════════════════════════════ */
static void notify_operators(const char *msg)
{
    pthread_mutex_lock(&operators_mutex);
    for (int i = 0; i < MAX_OPERATORS; i++) {
        if (operators[i].active)
            send(operators[i].fd, msg, strlen(msg), MSG_NOSIGNAL);
    }
    pthread_mutex_unlock(&operators_mutex);
}

/* ═══════════════════════════════════════════════════════
 *  CLIENTE HTTP → SERVICIO DE AUTENTICACION EXTERNO
 * ═══════════════════════════════════════════════════════ */
static int check_auth(const char *username, const char *password)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", auth_port);

    if (getaddrinfo(auth_host, port_str, &hints, &res) != 0) {
        log_error_msg("No se puede resolver el host del servicio de autenticacion");
        return 0;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return 0; }

    struct timeval tv = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        log_error_msg("No se puede conectar al servicio de autenticacion");
        return 0;
    }
    freeaddrinfo(res);

    char request[512];
    snprintf(request, sizeof(request),
        "GET /auth?user=%s&pass=%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        username, password, auth_host);
    send(sock, request, strlen(request), 0);

    char response[1024];
    int received = recv(sock, response, sizeof(response) - 1, 0);
    close(sock);

    if (received <= 0) return 0;
    response[received] = '\0';
    return (strstr(response, "200") != NULL);
}

/* ═══════════════════════════════════════════════════════
 *  PROTOCOLO — ESTADO DEL SISTEMA
 * ═══════════════════════════════════════════════════════ */
static void build_status_response(char *out, size_t out_size)
{
    strncpy(out, "SENSORS", out_size - 1);
    pthread_mutex_lock(&sensors_mutex);
    for (int i = 0; i < sensor_count; i++) {
        char entry[256];
        snprintf(entry, sizeof(entry), "|%s:%s:%.2f:%s:%s",
            sensors[i].id,
            sensors[i].type,
            sensors[i].last_value,
            sensors[i].unit,
            sensors[i].status == STATUS_ONLINE ? "ONLINE" : sensors[i].status == STATUS_PAUSED ? "PAUSED" : "OFFLINE");
        strncat(out, entry, out_size - strlen(out) - 1);
    }
    pthread_mutex_unlock(&sensors_mutex);
    strncat(out, "\n", out_size - strlen(out) - 1);
}

/* ═══════════════════════════════════════════════════════
 *  HILO: MANEJADOR DE SENSOR (TCP :5000)
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
} ClientArgs;

static void *handle_sensor(void *arg)
{
    ClientArgs *a = (ClientArgs *)arg;
    int  fd   = a->fd;
    char ip[INET_ADDRSTRLEN];
    int  port = a->port;
    strncpy(ip, a->ip, sizeof(ip));
    free(a);

    char buf[MAX_BUF];
    char sensor_id[64] = "";
    int  registered    = 0;

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;

        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) continue;

        char work[MAX_BUF];
        strncpy(work, buf, sizeof(work) - 1);

        char response[MAX_BUF] = "";
        char *cmd = strtok(work, "|");
        if (!cmd) continue;

        /* ── REGISTER ── */
        if (strcmp(cmd, "REGISTER") == 0) {
            char *id  = strtok(NULL, "|");
            char *typ = strtok(NULL, "|");
            char *loc = strtok(NULL, "|");

            if (!id || !typ || !loc) {
                snprintf(response, sizeof(response), "ERROR|Formato invalido\n");
            } else {
                pthread_mutex_lock(&sensors_mutex);
                Sensor *s = find_sensor(id);
                if (!s && sensor_count < MAX_SENSORS)
                    s = &sensors[sensor_count++];

                if (s) {
                    strncpy(s->id,       id,  sizeof(s->id)       - 1);
                    strncpy(s->type,     typ, sizeof(s->type)     - 1);
                    strncpy(s->location, loc, sizeof(s->location) - 1);
                    s->status    = STATUS_ONLINE;
                    s->tcp_fd    = fd;
                    s->last_seen = time(NULL);
                    s->ping_miss = 0;
                    strncpy(sensor_id, id, sizeof(sensor_id) - 1);
                    registered = 1;
                    snprintf(response, sizeof(response), "OK|%s\n", id);
                } else {
                    snprintf(response, sizeof(response), "ERROR|Servidor lleno\n");
                }
                pthread_mutex_unlock(&sensors_mutex);
            }

        /* ── DATA ── */
        } else if (strcmp(cmd, "DATA") == 0) {
            if (!registered) {
                snprintf(response, sizeof(response), "ERROR|Sensor no registrado\n");
            } else {
                char *id      = strtok(NULL, "|");
                char *val_str = strtok(NULL, "|");
                char *unit    = strtok(NULL, "|");
                /* timestamp opcional */

                if (!id || !val_str || !unit) {
                    snprintf(response, sizeof(response), "ERROR|Formato invalido\n");
                } else {
                    double value = atof(val_str);
                    const char *alert_desc = NULL;

                    pthread_mutex_lock(&sensors_mutex);
                    Sensor *s = find_sensor(id);
                    if (s) {
                        s->last_value = value;
                        strncpy(s->unit, unit, sizeof(s->unit) - 1);
                        s->last_seen  = time(NULL);
                        s->status     = STATUS_ONLINE;
                        alert_desc    = check_alert(s->type, value);
                    }
                    pthread_mutex_unlock(&sensors_mutex);

                    if (alert_desc) {
                        snprintf(response, sizeof(response), "ALERT|%s\n", alert_desc);
                        char notify[MAX_BUF];
                        snprintf(notify, sizeof(notify),
                            "NOTIFY|%s|%s|%.2f|%ld\n",
                            id, alert_desc, value, (long)time(NULL));
                        notify_operators(notify);
                    } else {
                        snprintf(response, sizeof(response), "OK\n");
                    }
                }
            }

        /* ── STATUS ── */
        } else if (strcmp(cmd, "STATUS") == 0) {
            build_status_response(response, sizeof(response));

        } else {
            snprintf(response, sizeof(response), "ERROR|Comando desconocido\n");
        }

        log_entry(ip, port, buf, response);
        send(fd, response, strlen(response), MSG_NOSIGNAL);
    }

    /* Sensor desconectado */
    if (registered) {
        pthread_mutex_lock(&sensors_mutex);
        Sensor *s = find_sensor(sensor_id);
        if (s) s->status = STATUS_OFFLINE;
        pthread_mutex_unlock(&sensors_mutex);

        char notify[256];
        snprintf(notify, sizeof(notify),
            "NOTIFY|%s|SENSOR_OFFLINE|0|%ld\n", sensor_id, (long)time(NULL));
        notify_operators(notify);

        char logmsg[128];
        snprintf(logmsg, sizeof(logmsg), "Sensor %s desconectado", sensor_id);
        log_error_msg(logmsg);
    }

    close(fd);
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  HILO: MANEJADOR DE OPERADOR (TCP :5001)
 * ═══════════════════════════════════════════════════════ */
static void *handle_operator(void *arg)
{
    ClientArgs *a = (ClientArgs *)arg;
    int  fd   = a->fd;
    char ip[INET_ADDRSTRLEN];
    int  port = a->port;
    strncpy(ip, a->ip, sizeof(ip));
    free(a);

    /* Registrar operador */
    int op_idx = -1;
    pthread_mutex_lock(&operators_mutex);
    for (int i = 0; i < MAX_OPERATORS; i++) {
        if (!operators[i].active) {
            operators[i].fd     = fd;
            operators[i].port   = port;
            operators[i].active = 1;
            strncpy(operators[i].ip, ip, sizeof(operators[i].ip));
            op_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&operators_mutex);

    if (op_idx < 0) {
        const char *err = "ERROR|Servidor lleno\n";
        send(fd, err, strlen(err), 0);
        close(fd);
        return NULL;
    }

    char buf[MAX_BUF];
    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;

        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) continue;

        char work[MAX_BUF];
        strncpy(work, buf, sizeof(work) - 1);

        char response[MAX_BUF] = "";
        char *cmd = strtok(work, "|");
        if (!cmd) continue;

        if (strcmp(cmd, "STATUS") == 0) {
            build_status_response(response, sizeof(response));
        } else {
            snprintf(response, sizeof(response), "ERROR|Comando desconocido\n");
        }

        log_entry(ip, port, buf, response);
        if (strlen(response) > 0)
            send(fd, response, strlen(response), MSG_NOSIGNAL);
    }

    pthread_mutex_lock(&operators_mutex);
    operators[op_idx].active = 0;
    pthread_mutex_unlock(&operators_mutex);

    log_error_msg("Operador desconectado");
    close(fd);
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  HTTP — utilidades
 * ═══════════════════════════════════════════════════════ */
static char *read_file_contents(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void http_respond(int fd, int code, const char *ctype, const char *body)
{
    const char *status =
        code == 200 ? "OK" :
        code == 401 ? "Unauthorized" :
        code == 404 ? "Not Found" :
        code == 405 ? "Method Not Allowed" : "Error";

    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status, ctype, body_len);

    send(fd, header, strlen(header), MSG_NOSIGNAL);
    if (body && body_len > 0)
        send(fd, body, body_len, MSG_NOSIGNAL);
}

/* ═══════════════════════════════════════════════════════
 *  HILO: MANEJADOR HTTP (TCP :8080)
 * ═══════════════════════════════════════════════════════ */
static void *handle_http(void *arg)
{
    ClientArgs *a = (ClientArgs *)arg;
    int  fd   = a->fd;
    char ip[INET_ADDRSTRLEN];
    int  port = a->port;
    strncpy(ip, a->ip, sizeof(ip));
    free(a);

    char buf[MAX_BUF * 2];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    buf[n] = '\0';

    char method[16] = "", path[512] = "", version[16] = "";
    sscanf(buf, "%15s %511s %15s", method, path, version);

    log_entry(ip, port, path, "HTTP");

    /* Solo GET */
    if (strcmp(method, "GET") != 0) {
        http_respond(fd, 405, "text/plain", "Method Not Allowed");
        close(fd);
        return NULL;
    }

    /* Separar path de query string */
    char path_only[512], query_str[512] = "";
    char *q = strchr(path, '?');
    if (q) {
        size_t plen = (size_t)(q - path);
        if (plen >= sizeof(path_only)) plen = sizeof(path_only) - 1;
        strncpy(path_only, path, plen);
        path_only[plen] = '\0';
        strncpy(query_str, q + 1, sizeof(query_str) - 1);
    } else {
        strncpy(path_only, path, sizeof(path_only) - 1);
    }

    /* ── GET / o /login (sin parámetros) → formulario ── */
    if (strcmp(path_only, "/") == 0 ||
        strcmp(path_only, "/login") == 0 ||
        strcmp(path_only, "/index.html") == 0)
    {
        if (strlen(query_str) == 0) {
            char *html = read_file_contents("web/index.html");
            if (html) { http_respond(fd, 200, "text/html", html); free(html); }
            else       { http_respond(fd, 404, "text/plain", "index.html not found"); }
            close(fd);
            return NULL;
        }

        /* Tiene query: extraer user y pass */
        char username[128] = "", password[128] = "";
        char tmp[512];
        strncpy(tmp, query_str, sizeof(tmp) - 1);
        char *tok = strtok(tmp, "&");
        while (tok) {
            if (strncmp(tok, "user=", 5) == 0)
                strncpy(username, tok + 5, sizeof(username) - 1);
            else if (strncmp(tok, "pass=", 5) == 0)
                strncpy(password, tok + 5, sizeof(password) - 1);
            tok = strtok(NULL, "&");
        }

        if (check_auth(username, password)) {
            char *html = read_file_contents("web/dashboard.html");
            if (html) { http_respond(fd, 200, "text/html", html); free(html); }
            else       { http_respond(fd, 200, "text/html", "<html><body><h1>Dashboard</h1></body></html>"); }
        } else {
            const char *redir =
                "HTTP/1.1 302 Found\r\n"
                "Location: /login?error=1\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            send(fd, redir, strlen(redir), MSG_NOSIGNAL);
        }

    /* ── GET /dashboard ── */
    } else if (strcmp(path_only, "/dashboard") == 0 ||
               strcmp(path_only, "/dashboard.html") == 0) {
        char *html = read_file_contents("web/dashboard.html");
        if (html) { http_respond(fd, 200, "text/html", html); free(html); }
        else       { http_respond(fd, 404, "text/plain", "dashboard.html not found"); }

    /* ── GET /sensors → JSON ── */
    } else if (strcmp(path_only, "/sensors") == 0) {
        char json[MAX_BUF];
        strcpy(json, "[");
        pthread_mutex_lock(&sensors_mutex);
        for (int i = 0; i < sensor_count; i++) {
            char entry[256];
            snprintf(entry, sizeof(entry),
                "%s{\"id\":\"%s\",\"type\":\"%s\",\"location\":\"%s\",\"value\":%.2f,\"unit\":\"%s\",\"status\":\"%s\"}",
                i > 0 ? "," : "",
                sensors[i].id, sensors[i].type, sensors[i].location,
                sensors[i].last_value, sensors[i].unit,
                sensors[i].status == STATUS_ONLINE ? "ONLINE" : sensors[i].status == STATUS_PAUSED ? "PAUSED" : "OFFLINE");
            strncat(json, entry, sizeof(json) - strlen(json) - 1);
        }
        pthread_mutex_unlock(&sensors_mutex);
        strncat(json, "]", sizeof(json) - strlen(json) - 1);
        http_respond(fd, 200, "application/json", json);

    /* ── GET /pause?sensor=X ── */
    } else if (strcmp(path_only, "/pause") == 0) {
        char sensor_id[64] = "";
        char tmp2[256]; strncpy(tmp2, query_str, sizeof(tmp2)-1);
        char *tok2 = strtok(tmp2, "&");
        while (tok2) {
            if (strncmp(tok2, "sensor=", 7) == 0) strncpy(sensor_id, tok2+7, sizeof(sensor_id)-1);
            tok2 = strtok(NULL, "&");
        }
        if (strlen(sensor_id) > 0) {
            pthread_mutex_lock(&sensors_mutex);
            Sensor *s = find_sensor(sensor_id);
            if (s) s->status = STATUS_PAUSED;
            pthread_mutex_unlock(&sensors_mutex);
            send_to_sensor(sensor_id, "PAUSE\n");
            char resp_json[128];
            snprintf(resp_json, sizeof(resp_json), "{\"status\":\"paused\",\"sensor\":\"%s\"}", sensor_id);
            http_respond(fd, 200, "application/json", resp_json);
        } else {
            http_respond(fd, 400, "application/json", "{\"error\":\"missing sensor\"}");
        }

    /* ── GET /resume?sensor=X ── */
    } else if (strcmp(path_only, "/resume") == 0) {
        char sensor_id[64] = "";
        char tmp3[256]; strncpy(tmp3, query_str, sizeof(tmp3)-1);
        char *tok3 = strtok(tmp3, "&");
        while (tok3) {
            if (strncmp(tok3, "sensor=", 7) == 0) strncpy(sensor_id, tok3+7, sizeof(sensor_id)-1);
            tok3 = strtok(NULL, "&");
        }
        if (strlen(sensor_id) > 0) {
            pthread_mutex_lock(&sensors_mutex);
            Sensor *s = find_sensor(sensor_id);
            if (s) s->status = STATUS_ONLINE;
            pthread_mutex_unlock(&sensors_mutex);
            send_to_sensor(sensor_id, "RESUME\n");
            char resp_json[128];
            snprintf(resp_json, sizeof(resp_json), "{\"status\":\"resumed\",\"sensor\":\"%s\"}", sensor_id);
            http_respond(fd, 200, "application/json", resp_json);
        } else {
            http_respond(fd, 400, "application/json", "{\"error\":\"missing sensor\"}");
        }

    /* ── GET /threshold?type=X&value=Y ── */
    } else if (strcmp(path_only, "/threshold") == 0) {
        char t_type[32] = "", t_val[32] = "";
        char tmp4[256]; strncpy(tmp4, query_str, sizeof(tmp4)-1);
        char *tok4 = strtok(tmp4, "&");
        while (tok4) {
            if (strncmp(tok4, "type=",  5) == 0) strncpy(t_type, tok4+5, sizeof(t_type)-1);
            if (strncmp(tok4, "value=", 6) == 0) strncpy(t_val,  tok4+6, sizeof(t_val)-1);
            tok4 = strtok(NULL, "&");
        }
        double new_val = atof(t_val);
        pthread_mutex_lock(&thresh_mutex);
        if      (strcmp(t_type, "temperatura") == 0) THRESH_TEMP     = new_val;
        else if (strcmp(t_type, "vibracion")   == 0) THRESH_VIB      = new_val;
        else if (strcmp(t_type, "energia")     == 0) THRESH_ENERGY   = new_val;
        else if (strcmp(t_type, "presion")     == 0) THRESH_PRESSURE = new_val;
        pthread_mutex_unlock(&thresh_mutex);
        char resp_json[128];
        snprintf(resp_json, sizeof(resp_json), "{\"type\":\"%s\",\"value\":%.2f}", t_type, new_val);
        http_respond(fd, 200, "application/json", resp_json);

    /* ── GET /thresholds ── */
    } else if (strcmp(path_only, "/thresholds") == 0) {
        pthread_mutex_lock(&thresh_mutex);
        char resp_json[256];
        snprintf(resp_json, sizeof(resp_json),
            "{\"temperatura\":%.2f,\"vibracion\":%.2f,\"energia\":%.2f,\"presion\":%.2f}",
            THRESH_TEMP, THRESH_VIB, THRESH_ENERGY, THRESH_PRESSURE);
        pthread_mutex_unlock(&thresh_mutex);
        http_respond(fd, 200, "application/json", resp_json);

    } else {
        http_respond(fd, 404, "text/plain", "Not Found");
    }

    close(fd);
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  HILO: UDP HEALTH CHECK (:5555)
 * ═══════════════════════════════════════════════════════ */
static void *udp_health_check(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { log_error_msg("UDP socket() fallo"); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT_UDP);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error_msg("UDP bind() fallo");
        close(sock);
        return NULL;
    }

    char buf[256];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&client_addr, &client_len);
        if (n <= 0) continue;

        buf[strcspn(buf, "\r\n")] = '\0';

        char cip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, cip, sizeof(cip));

        char work[256];
        strncpy(work, buf, sizeof(work) - 1);
        char *cmd = strtok(work, "|");
        char *id  = strtok(NULL, "|");

        if (cmd && strcmp(cmd, "PING") == 0 && id) {
            pthread_mutex_lock(&sensors_mutex);
            Sensor *s = find_sensor(id);
            if (s) {
                s->last_seen  = time(NULL);
                s->ping_miss  = 0;
                s->status     = STATUS_ONLINE;
            }
            pthread_mutex_unlock(&sensors_mutex);

            const char *pong = "PONG\n";
            sendto(sock, pong, strlen(pong), 0,
                   (struct sockaddr *)&client_addr, client_len);
            log_entry(cip, ntohs(client_addr.sin_port), buf, "PONG");
        }
    }

    close(sock);
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  HILO: DETECTOR DE SENSORES OFFLINE
 * ═══════════════════════════════════════════════════════ */
static void *check_offline_sensors(void *arg)
{
    (void)arg;
    while (1) {
        sleep(30);
        time_t now = time(NULL);
        pthread_mutex_lock(&sensors_mutex);
        for (int i = 0; i < sensor_count; i++) {
            if (sensors[i].status == STATUS_ONLINE &&
                (now - sensors[i].last_seen) > 60) {
                sensors[i].status = STATUS_OFFLINE;
                char notify[256];
                snprintf(notify, sizeof(notify),
                    "NOTIFY|%s|SENSOR_TIMEOUT|0|%ld\n",
                    sensors[i].id, (long)now);
                pthread_mutex_unlock(&sensors_mutex);
                notify_operators(notify);
                pthread_mutex_lock(&sensors_mutex);
            }
        }
        pthread_mutex_unlock(&sensors_mutex);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  ACEPTADOR TCP GENERICO
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    int   server_fd;
    void *(*handler)(void *);
} AcceptorArgs;

static void *tcp_acceptor(void *arg)
{
    AcceptorArgs *aa      = (AcceptorArgs *)arg;
    int           srv_fd  = aa->server_fd;
    void        *(*handler)(void *) = aa->handler;
    free(aa);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (1) {
        struct sockaddr_in ca;
        socklen_t          ca_len = sizeof(ca);
        int cfd = accept(srv_fd, (struct sockaddr *)&ca, &ca_len);
        if (cfd < 0) {
            if (errno != EINTR) log_error_msg("accept() fallo");
            continue;
        }

        ClientArgs *cargs = malloc(sizeof(ClientArgs));
        if (!cargs) { close(cfd); continue; }
        cargs->fd   = cfd;
        cargs->port = ntohs(ca.sin_port);
        inet_ntop(AF_INET, &ca.sin_addr, cargs->ip, sizeof(cargs->ip));

        pthread_t tid;
        pthread_create(&tid, &attr, handler, cargs);
    }

    pthread_attr_destroy(&attr);
    return NULL;
}

/* ═══════════════════════════════════════════════════════
 *  CREAR SOCKET TCP SERVIDOR
 * ═══════════════════════════════════════════════════════ */
static int create_tcp_server(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    if (listen(sock, 10) < 0) return -1;
    return sock;
}

/* ═══════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Uso: %s puerto archivoDeLogs\n", argv[0]);
        return 1;
    }

    /* El primer argumento es el puerto principal (sensores).
       Los demas puertos se derivan de la constante. */
    (void)atoi(argv[1]);  /* puerto recibido — se usa PORT_SENSORS por protocolo */

    /* Abrir log */
    log_fp = fopen(argv[2], "a");
    if (!log_fp) {
        fprintf(stderr, "Error abriendo log: %s\n", argv[2]);
        return 1;
    }

    /* Configurar host de autenticacion desde variable de entorno */
    char *env_host = getenv("AUTH_HOST");
    char *env_port = getenv("AUTH_PORT");
    if (env_host) strncpy(auth_host, env_host, sizeof(auth_host) - 1);
    if (env_port) auth_port = atoi(env_port);

    signal(SIGPIPE, SIG_IGN);

    /* Crear sockets TCP */
    int sensor_fd   = create_tcp_server(PORT_SENSORS);
    int operator_fd = create_tcp_server(PORT_OPERATORS);
    int http_fd     = create_tcp_server(PORT_HTTP);

    if (sensor_fd < 0 || operator_fd < 0 || http_fd < 0) {
        log_error_msg("Error creando sockets TCP");
        return 1;
    }

    printf("========================================\n");
    printf("  Servidor IoT - Monitoreo Distribuido  \n");
    printf("========================================\n");
    printf("  Sensores   TCP : %d\n", PORT_SENSORS);
    printf("  Operadores TCP : %d\n", PORT_OPERATORS);
    printf("  HTTP       TCP : %d\n", PORT_HTTP);
    printf("  HealthCheck UDP: %d\n", PORT_UDP);
    printf("  Auth service   : %s:%d\n", auth_host, auth_port);
    printf("  Log file       : %s\n", argv[2]);
    printf("========================================\n");

    /* Hilo UDP */
    pthread_t udp_tid;
    pthread_create(&udp_tid, NULL, udp_health_check, NULL);
    pthread_detach(udp_tid);

    /* Hilo detector offline */
    pthread_t offline_tid;
    pthread_create(&offline_tid, NULL, check_offline_sensors, NULL);
    pthread_detach(offline_tid);

    /* Aceptadores TCP en hilos propios */
    AcceptorArgs *sa = malloc(sizeof(AcceptorArgs));
    sa->server_fd = sensor_fd; sa->handler = handle_sensor;
    pthread_t st; pthread_create(&st, NULL, tcp_acceptor, sa); pthread_detach(st);

    AcceptorArgs *oa = malloc(sizeof(AcceptorArgs));
    oa->server_fd = operator_fd; oa->handler = handle_operator;
    pthread_t ot; pthread_create(&ot, NULL, tcp_acceptor, oa); pthread_detach(ot);

    /* HTTP corre en el hilo principal */
    AcceptorArgs *ha = malloc(sizeof(AcceptorArgs));
    ha->server_fd = http_fd; ha->handler = handle_http;
    tcp_acceptor(ha);   /* bloquea aqui */

    fclose(log_fp);
    return 0;
}
