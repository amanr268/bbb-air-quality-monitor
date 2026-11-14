#ifndef AQM_MQTT_H
#define AQM_MQTT_H

/*
 * MQTT publisher (libmosquitto). Connection, keep-alive and reconnects run in
 * libmosquitto's own network thread, so publishing never blocks the caller.
 * Settings come from a "key = value" file (config/mqtt.conf):
 *   enabled = 1 | host = 192.168.1.10 | port = 1883 | topic = aqm/bbb-aqi
 *   keepalive = 30 | username / password (optional)
 * Topics under <topic>/:  status (retained "online"/"offline", also the
 * last will), sample (QoS 0), avg (QoS 1, retained), alarm (QoS 1).
 * Built without libmosquitto-dev, every call is a no-op that reports "disabled".
 */

typedef struct aqm_mqtt aqm_mqtt_t;

/* NULL if disabled or not configured (reason logged once) */
aqm_mqtt_t *aqm_mqtt_start(const char *conf_path);
/* qos 0/1; returns 0, or -1 when not connected (message dropped) */
int         aqm_mqtt_publish(aqm_mqtt_t *m, const char *subtopic, const char *payload,
                             int qos, int retain);
int         aqm_mqtt_connected(const aqm_mqtt_t *m);
/* publishes "offline", disconnects, joins the network thread */
void        aqm_mqtt_stop(aqm_mqtt_t *m);

#endif
