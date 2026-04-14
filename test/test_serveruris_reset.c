/*
 * Reproducer: MQTTAsync_reconnect does not restart the server URI cycling from the beginning.
 *
 * Expected behavior: when a client is configured with serverURIs=[A, B] and
 * MQTTAsync_reconnect is called, it should iterate over all configured URIs starting
 * from the first one, retrying until one succeeds.
 *
 * Actual behavior: MQTTAsync_reconnect doesnt modify the serverUri index, 
 * and reconnecting is only attempted again the last serverUri in the list. 
 *
 * Workaround: MQTTAsync_disconnect followed by MQTTAsync_connect resets the URI
 * index to 0 and iterates from the beginning.
 *
 * Dependencies: paho-mqtt-c (async), mosquitto broker, toxiproxy 2.x
 *
 * Setup (docker/podman):
 *   docker compose up -d   # from the repo root, uses compose.yaml
 * Proxy setup is performed automatically by the test suite.
 */


#include <MQTTAsync.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#define LOGA_DEBUG 0
#define LOGA_INFO 1

static int tests = 0;
static int failures = 0;
FILE* xml;

typedef struct {
    const char* connection;
    int verbose;
    int test_no;
} Options;

Options options = {
    .connection = "tcp://localhost:1885",
    .verbose = 0,
    .test_no = 1,
};

void MyLog(int LOGA_level, const char* format, ...)
{
    static char msg_buf[256];
    va_list args;
    struct timeval ts;
    struct tm timeinfo;

    if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
        return;

    gettimeofday(&ts, NULL);
    localtime_r(&ts.tv_sec, &timeinfo);
    strftime(msg_buf, 80, "%Y%m%d %H%M%S", &timeinfo);
    sprintf(&msg_buf[strlen(msg_buf)], ".%.3lu ", ts.tv_usec / 1000L);

    va_start(args, format);
    vsnprintf(&msg_buf[strlen(msg_buf)], sizeof(msg_buf) - strlen(msg_buf), format, args);
    va_end(args);

    printf("%s\n", msg_buf);
    fflush(stdout);
}

/* ---- assert ---- */
#include <stdarg.h>
static void myassert(const char* filename, int lineno, const char* description, int value, const char* format, ...)
{
    ++tests;
    if (!value) {
        va_list args;
        ++failures;
        MyLog(LOGA_INFO, "Assertion failed, file %s, line %d, description: %s", filename, lineno, description);
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        if (xml)
            fprintf(xml, "<failure type=\"%s\">file %s, line %d </failure>\n", description, filename, lineno);
        exit(1);
    } else {
        MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s", filename, lineno, description);
    }
}
#define assert(desc, cond, fmt, ...) myassert(__FILE__, __LINE__, desc, cond, fmt, ##__VA_ARGS__)

// Minimal HTTP POST/GET for Toxiproxy control
// Returns 0 on success, -1 on error
static int toxiproxy_http(const char *method, const char *path, const char *body) {
    int sockfd, ret = -1;
    struct hostent *server;
    struct sockaddr_in serv_addr;
    char request[1024], response[1024];
    int port = 8474;
    const char *host = "127.0.0.1";

    server = gethostbyname(host);
    if (!server) return -1;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd); return -1;
    }
    int len = 0;
    if (body) {
        len = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            method, path, host, strlen(body), body);
    } else {
        len = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, host);
    }
    if (write(sockfd, request, len) < 0) { close(sockfd); return -1; }
    // Read response (ignore content)
    read(sockfd, response, sizeof(response)-1);
    close(sockfd);
    return 0;
}
static void toxi(const char *method, const char *path, const char *body) {
    if (toxiproxy_http(method, path, body) != 0) {
        fprintf(stderr, "toxiproxy request failed: %s %s\n", method, path);
        exit(1);
    }
}

static void setup_toxiproxy(void) {
    const char *body =
        "[{\"name\":\"mqtt1\",\"listen\":\"0.0.0.0:1885\",\"upstream\":\"mosquitto:1883\",\"enabled\":true},"
        " {\"name\":\"mqtt2\",\"listen\":\"0.0.0.0:1886\",\"upstream\":\"mosquitto:1883\",\"enabled\":true}]";
    if (toxiproxy_http("POST", "/populate", body) != 0) {
        fprintf(stderr, "toxiproxy setup failed — is toxiproxy running on :8474?\n");
        exit(1);
    }
    MyLog(LOGA_INFO, "toxiproxy proxies mqtt1 (1885) and mqtt2 (1886) configured");
}

/* ---- callbacks ---- */

typedef struct {
    volatile bool connected;
    volatile bool connect_failed;
} conn_ctx_t;

static void on_connect(void *ctx, MQTTAsync_successData *r) {
    (void)r; ((conn_ctx_t *)ctx)->connected = true;
}
static void on_connect_fail(void *ctx, MQTTAsync_failureData *r) {
    (void)r; ((conn_ctx_t *)ctx)->connect_failed = true;
}
static void on_conn_lost(void *ctx, char *cause) {
    (void)ctx; (void)cause;
}
static void on_disconnect(void *ctx, MQTTAsync_successData *r) {
    (void)r; *(int *)ctx = 1;
}

/* ---- wait helpers ---- */

#define WAIT_TRUE(expr, ms) do { \
    for (int _i = 0; _i < (ms)/10 && !(expr); _i++) usleep(10000); \
} while(0)

/* ---- test ---- */

static int test_serveruris_reset(void) {
        const char *uris[] = {"tcp://localhost:1885", "tcp://localhost:1886"};
        conn_ctx_t conn = {0};

        MQTTAsync client;
        MQTTAsync_create(&client, uris[0], "repro_client", MQTTCLIENT_PERSISTENCE_NONE, NULL);
        MQTTAsync_setCallbacks(client, &conn, on_conn_lost, NULL, NULL);

        MQTTAsync_connectOptions co = MQTTAsync_connectOptions_initializer;
        co.cleansession     = 1;
        co.automaticReconnect = 0;
        co.serverURIs       = (char **)uris;
        co.serverURIcount   = 2;
        co.onSuccess        = on_connect;
        co.onFailure        = on_connect_fail;
        co.context          = &conn;

        fprintf(xml, "<testcase classname=\"test_serveruris_reset\" name=\"serveruris_reset\"");
        MyLog(LOGA_INFO, "Starting test_serveruris_reset");

        /* start with only 1885 up */
        toxi("POST", "/proxies/mqtt1",  "{\"enabled\":true}");
        toxi("POST", "/proxies/mqtt2", "{\"enabled\":false}");

        MQTTAsync_connect(client, &co);
        WAIT_TRUE(conn.connected || conn.connect_failed, 3000);
        assert("initial connect on 1885", MQTTAsync_isConnected(client), "\n");

        /* take 1885 down */
        toxi("POST", "/proxies/mqtt1", "{\"enabled\":false}");
        WAIT_TRUE(!MQTTAsync_isConnected(client), 3000);
        assert("disconnected after 1885 went down", !MQTTAsync_isConnected(client), "\n");

        /* bring 1886 up, reconnect — paho tries next URI (1886), succeeds */
        toxi("POST", "/proxies/mqtt2", "{\"enabled\":true}");
        conn.connected = false; conn.connect_failed = false;
        MQTTAsync_reconnect(client);
        WAIT_TRUE(conn.connected || conn.connect_failed, 3000);
        assert("reconnected via 1886", MQTTAsync_isConnected(client), "\n");

        /* take 1886 down */
        toxi("POST", "/proxies/mqtt2", "{\"enabled\":false}");
        WAIT_TRUE(!MQTTAsync_isConnected(client), 3000);
        assert("disconnected after 1886 went down", !MQTTAsync_isConnected(client), "\n");

        /* reconnect — restart cycle from 1885 */
        conn.connected = false; conn.connect_failed = false;
        toxi("POST", "/proxies/mqtt1",  "{\"enabled\":true}");
        conn.connected = false; conn.connect_failed = false;
        MQTTAsync_reconnect(client);
        WAIT_TRUE(conn.connected || conn.connect_failed, 3000);
        assert("reconnect, restart cycle from 1885", MQTTAsync_isConnected(client), "\n");

        MQTTAsync_destroy(&client);
        MyLog(LOGA_INFO, "TEST: test_serveruris_reset %s. %d assertions run, %d failures.", (failures == 0) ? "passed" : "failed", tests, failures);
        fprintf(xml, " time=\"0\" >\n");
        fprintf(xml, "</testcase>\n");
        return failures;
    }

    void getopts(int argc, char** argv) {
        int count = 1;
        while (count < argc) {
            if (strcmp(argv[count], "--connection") == 0) {
                if (++count < argc)
                    options.connection = argv[count];
                else
                    exit(EXIT_FAILURE);
            } else if (strcmp(argv[count], "--verbose") == 0) {
                options.verbose = 1;
            }
            count++;
        }
    }

int main(int argc, char** argv) {
    int rc = 0;
    xml = fopen("TEST-test_serveruris_reset.xml", "w");
    fprintf(xml, "<testsuite name=\"test_serveruris_reset\" tests=\"1\">\n");

    setenv("MQTT_C_CLIENT_TRACE", "ON", 1);
    setenv("MQTT_C_CLIENT_TRACE_LEVEL", "ERROR", 1);

    getopts(argc, argv);

    setup_toxiproxy();

    rc = test_serveruris_reset();

    fprintf(xml, "</testsuite>\n");
    fclose(xml);
    return rc;
}