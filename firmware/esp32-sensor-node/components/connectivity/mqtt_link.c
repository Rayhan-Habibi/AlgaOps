/* mqtt_link.c — see mqtt_link.h */

#include "mqtt_link.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t            s_acks;       /* msg ids of MQTT_EVENT_PUBLISHED */
static volatile bool            s_connected;
static volatile uint32_t        s_epoch;
static volatile uint32_t        s_disconnects;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_epoch++;
            ESP_LOGI(TAG, "connected to the broker");
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) s_disconnects++;
            s_connected = false;
            ESP_LOGW(TAG, "disconnected; the client reconnects automatically");
            break;
        case MQTT_EVENT_PUBLISHED: {
            int mid = ev->msg_id;
            xQueueSend(s_acks, &mid, 0);
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "transport error (type %d)",
                     ev->error_handle ? (int)ev->error_handle->error_type : -1);
            break;
        default:
            break;
    }
}

esp_err_t mqtt_link_start(const mqtt_link_config_t *cfg)
{
    if (!cfg || !cfg->uri || !cfg->client_id) return ESP_ERR_INVALID_ARG;
    s_acks = xQueueCreate(16, sizeof(int));
    if (!s_acks) return ESP_ERR_NO_MEM;
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = cfg->uri,
        .credentials.client_id = cfg->client_id,
        .credentials.username = (cfg->username && cfg->username[0]) ? cfg->username : NULL,
        .credentials.authentication.password =
            (cfg->password && cfg->password[0]) ? cfg->password : NULL,
        .session.keepalive = 30,
        .session.last_will.topic = cfg->lwt_topic,
        .session.last_will.msg = cfg->lwt_payload,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = 2048,
    };
    if (cfg->ca_pem) mc.broker.verification.certificate = cfg->ca_pem;
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    return esp_mqtt_client_start(s_client);
}

bool mqtt_link_connected(void) { return s_connected; }

uint32_t mqtt_link_epoch(void) { return s_epoch; }

uint32_t mqtt_link_disconnects(void) { return s_disconnects; }

int mqtt_link_publish(const char *topic, const char *payload, int len, int qos, bool retain)
{
    if (!s_client || !s_connected) return -1;
    return esp_mqtt_client_publish(s_client, topic, payload, len, qos, retain ? 1 : 0);
}

bool mqtt_link_wait_ack(int msg_id, uint32_t timeout_ms)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(end - now) <= 0) return false;
        int mid = -1;
        if (xQueueReceive(s_acks, &mid, end - now) == pdTRUE && mid == msg_id) return true;
        /* other ids are late acknowledgements of earlier attempts: ignored */
    }
}
