#include "aqm/mqtt.h"

#include "aqm/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef AQM_HAVE_MQTT
#include <mosquitto.h>
#include <pthread.h>

struct aqm_mqtt {
    struct mosquitto *mosq;
    char  host[64], topic[64], user[64], pass[64];
    int   port, keepalive;
    int   connected;            /* written by the network thread */
    int   cmd_power, cmd_bright;   /* OLED commands from the app, -1 = none */
    unsigned long dropped;      /* publishes while offline */
};

/* "key = value" lines, '#' comments */
static int load_conf(struct aqm_mqtt *m, const char *path, int *enabled)
{
    char line[160], key[32], val[96];
    FILE *f = fopen(path, "r");

    if (!f)
        return -errno;
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');

        if (h)
            *h = '\0';
        if (sscanf(line, " %31[^= ] = %95s", key, val) != 2)
            continue;
        if (!strcmp(key, "enabled"))        *enabled = atoi(val);
        else if (!strcmp(key, "host"))      snprintf(m->host, sizeof(m->host), "%.63s", val);
        else if (!strcmp(key, "port"))      m->port = atoi(val);
        else if (!strcmp(key, "topic"))     snprintf(m->topic, sizeof(m->topic), "%.63s", val);
        else if (!strcmp(key, "keepalive")) m->keepalive = atoi(val);
        else if (!strcmp(key, "username"))  snprintf(m->user, sizeof(m->user), "%.63s", val);
        else if (!strcmp(key, "password"))  snprintf(m->pass, sizeof(m->pass), "%.63s", val);
    }
    fclose(f);
    return 0;
}

static void topic_of(const struct aqm_mqtt *m, const char *sub, char *buf, size_t n)
{
    snprintf(buf, n, "%s/%s", m->topic, sub);
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    struct aqm_mqtt *m = obj;
    char t[96];

    if (rc != 0) {
        aqm_log("report", "MQTT %s:%d refused: %s", m->host, m->port,
                mosquitto_connack_string(rc));
        return;
    }
    __atomic_store_n(&m->connected, 1, __ATOMIC_RELEASE);
    topic_of(m, "status", t, sizeof(t));
    mosquitto_publish(mosq, NULL, t, 6, "online", 1, true);
    topic_of(m, "cmd/oled/#", t, sizeof(t));
    mosquitto_subscribe(mosq, NULL, t, 1);      /* retained brightness arrives at once */
    aqm_log("report", "MQTT connected to %s:%d, publishing under %s/", m->host, m->port, m->topic);
}

/* network thread: only stores the command; report applies it in its loop */
static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    struct aqm_mqtt *m = obj;
    char v[16] = "";
    const char *t = msg->topic;
    size_t n = strlen(t);

    (void)mosq;
    if (msg->payloadlen > 0)
        snprintf(v, sizeof(v), "%.*s", msg->payloadlen < 15 ? msg->payloadlen : 15,
                 (const char *)msg->payload);
    if (n >= 10 && !strcmp(t + n - 10, "oled/power")) {
        if (!strcmp(v, "on") || !strcmp(v, "off"))
            __atomic_store_n(&m->cmd_power, v[1] == 'n', __ATOMIC_RELEASE);
    } else if (n >= 15 && !strcmp(t + n - 15, "oled/brightness")) {
        char *end;
        long b = strtol(v, &end, 10);

        if (end != v && b >= 0 && b <= 100)
            __atomic_store_n(&m->cmd_bright, (int)b, __ATOMIC_RELEASE);
    }
}

int aqm_mqtt_take_oled(aqm_mqtt_t *m, int *power, int *brightness)
{
    if (!m)
        return 0;
    *power = __atomic_exchange_n(&m->cmd_power, -1, __ATOMIC_ACQ_REL);
    *brightness = __atomic_exchange_n(&m->cmd_bright, -1, __ATOMIC_ACQ_REL);
    return *power >= 0 || *brightness >= 0;
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    struct aqm_mqtt *m = obj;

    (void)mosq;
    if (__atomic_exchange_n(&m->connected, 0, __ATOMIC_ACQ_REL) && rc != 0)
        aqm_log("report", "MQTT connection lost (%s), reconnecting", mosquitto_strerror(rc));
}

aqm_mqtt_t *aqm_mqtt_start(const char *conf_path)
{
    struct aqm_mqtt *m = calloc(1, sizeof(*m));
    char id[64], host[32] = "bbb", will[96];
    pthread_attr_t attr;
    int enabled = 1, rc;

    if (!m)
        return NULL;
    m->port = 1883;
    m->keepalive = 30;
    m->cmd_power = m->cmd_bright = -1;
    snprintf(m->topic, sizeof(m->topic), "aqm/bbb-aqi");
    if ((rc = load_conf(m, conf_path, &enabled)) != 0 || !enabled || !m->host[0]) {
        aqm_log("report", "MQTT off (%s)", rc ? strerror(-rc) :
                !enabled ? "enabled = 0" : "no host set");
        free(m);
        return NULL;
    }

    mosquitto_lib_init();
    gethostname(host, sizeof(host) - 1);
    snprintf(id, sizeof(id), "aqm-%s-%d", host, (int)getpid());
    m->mosq = mosquitto_new(id, true, m);
    if (!m->mosq)
        goto fail;
    if (m->user[0])
        mosquitto_username_pw_set(m->mosq, m->user, m->pass[0] ? m->pass : NULL);
    topic_of(m, "status", will, sizeof(will));
    mosquitto_will_set(m->mosq, will, 7, "offline", 1, true);   /* broker says it for us */
    mosquitto_connect_callback_set(m->mosq, on_connect);
    mosquitto_disconnect_callback_set(m->mosq, on_disconnect);
    mosquitto_message_callback_set(m->mosq, on_message);
    mosquitto_reconnect_delay_set(m->mosq, 1, 10, true);        /* 1, 2, 4, 8, 10 s */
    /* report runs under mlockall(MCL_FUTURE): give the network thread a small
     * stack instead of the 8 MB default, which would all be locked in RAM */
    if (pthread_getattr_default_np(&attr) == 0) {
        pthread_attr_t small;
        pthread_attr_init(&small);
        pthread_attr_setstacksize(&small, 256 * 1024);
        pthread_setattr_default_np(&small);
        rc = mosquitto_loop_start(m->mosq);
        pthread_setattr_default_np(&attr);            /* restore */
        pthread_attr_destroy(&small);
        pthread_attr_destroy(&attr);
    } else {
        rc = mosquitto_loop_start(m->mosq);
    }
    if (rc != MOSQ_ERR_SUCCESS) {
        aqm_log("report", "MQTT thread: %s", mosquitto_strerror(rc));
        goto fail;
    }
    /* connect AFTER the thread runs: with libmosquitto 2.0, if the broker is
     * down at start-up (Mac asleep at boot) and connect_async is called first,
     * the thread never retries. Started first, it keeps reconnecting. */
    rc = mosquitto_connect_async(m->mosq, m->host, m->port, m->keepalive);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_ERRNO)
        aqm_log("report", "MQTT connect %s:%d: %s (will retry)", m->host, m->port,
                mosquitto_strerror(rc));
    aqm_log("report", "MQTT -> %s:%d topic %s/ (connecting)", m->host, m->port, m->topic);
    return m;

fail:
    if (m->mosq)
        mosquitto_destroy(m->mosq);
    mosquitto_lib_cleanup();
    free(m);
    return NULL;
}

int aqm_mqtt_connected(const aqm_mqtt_t *m)
{
    return m && __atomic_load_n(&m->connected, __ATOMIC_ACQUIRE);
}

int aqm_mqtt_publish(aqm_mqtt_t *m, const char *subtopic, const char *payload, int qos, int retain)
{
    char t[96];

    if (!m)
        return -1;
    if (!aqm_mqtt_connected(m)) {
        m->dropped++;
        return -1;
    }
    topic_of(m, subtopic, t, sizeof(t));
    return mosquitto_publish(m->mosq, NULL, t, (int)strlen(payload), payload, qos,
                             retain != 0) == MOSQ_ERR_SUCCESS ? 0 : -1;
}

void aqm_mqtt_stop(aqm_mqtt_t *m)
{
    char t[96];

    if (!m)
        return;
    if (aqm_mqtt_connected(m)) {
        topic_of(m, "status", t, sizeof(t));
        mosquitto_publish(m->mosq, NULL, t, 7, "offline", 1, true);
        usleep(100000);                         /* let the thread send it */
    }
    mosquitto_disconnect(m->mosq);
    /* force: during system shutdown the network may already be gone and a
     * graceful join could block until systemd kills the process */
    mosquitto_loop_stop(m->mosq, true);
    mosquitto_destroy(m->mosq);
    mosquitto_lib_cleanup();
    if (m->dropped)
        aqm_log("report", "MQTT: %lu messages dropped while offline", m->dropped);
    free(m);
}

#else  /* built without libmosquitto-dev */

aqm_mqtt_t *aqm_mqtt_start(const char *conf_path)
{
    (void)conf_path;
    aqm_log("report", "MQTT off (built without libmosquitto-dev)");
    return NULL;
}

int aqm_mqtt_publish(aqm_mqtt_t *m, const char *subtopic, const char *payload, int qos, int retain)
{
    (void)m; (void)subtopic; (void)payload; (void)qos; (void)retain;
    return -1;
}

int aqm_mqtt_connected(const aqm_mqtt_t *m)
{
    (void)m;
    return 0;
}

int aqm_mqtt_take_oled(aqm_mqtt_t *m, int *power, int *brightness)
{
    (void)m;
    *power = *brightness = -1;
    return 0;
}

void aqm_mqtt_stop(aqm_mqtt_t *m)
{
    (void)m;
}

#endif
