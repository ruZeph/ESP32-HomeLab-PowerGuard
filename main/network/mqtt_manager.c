#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "event_log.h"
#include "ups.h"
#include "wake_on_lan.h"

#define TAG "MQTT"

#define TOPIC_POWER_STATE    "powerguard/power_state"
#define TOPIC_HOST_STATE     "powerguard/host_state"
#define TOPIC_COUNTDOWN      "powerguard/countdown"
#define TOPIC_LAST_EVENT     "powerguard/last_event"
#define TOPIC_EVENT_LOG      "powerguard/event_log"
#define TOPIC_AVAILABILITY   "powerguard/availability"

#define TOPIC_CMD_CANCEL     "powerguard/command/cancel_shutdown"
#define TOPIC_CMD_FORCE_WOL  "powerguard/command/force_wol"
#define TOPIC_CMD_FORCE_SHUTDOWN "powerguard/command/force_shutdown"
#define TOPIC_CMD_CLEAR_LOG  "powerguard/command/clear_log"

#define HA_DISCOVERY_PREFIX  "homeassistant"

static esp_mqtt_client_handle_t s_client = NULL;

static char s_device_id[20];

static void build_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void publish(const char *topic, const char *data, int qos, int retain)
{
    if (s_client == NULL)
    {
        return;
    }

    esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
}

/* ---------- Home Assistant MQTT Discovery ---------- */

static void publish_ha_discovery_binary_sensor(
    const char *object_id,
    const char *name,
    const char *state_topic,
    const char *device_class,
    const char *payload_on,
    const char *payload_off)
{
    char topic[128];
    char payload[512];

    snprintf(topic, sizeof(topic),
             "%s/binary_sensor/powerguard/%s/config",
             HA_DISCOVERY_PREFIX, object_id);

    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"unique_id\":\"powerguard_%s_%s\","
             "\"state_topic\":\"%s\","
             "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
             "\"device_class\":\"%s\","
             "\"payload_on\":\"%s\","
             "\"payload_off\":\"%s\","
             "\"device\":{"
             "\"identifiers\":[\"powerguard_%s\"],"
             "\"name\":\"PowerGuard\","
             "\"manufacturer\":\"DIY\","
             "\"model\":\"ESP32 UPS Controller\""
             "}"
             "}",
             name, s_device_id, object_id, state_topic,
             device_class, payload_on, payload_off,
             s_device_id);

    publish(topic, payload, 1, 1);
}

static void publish_ha_discovery_sensor(
    const char *object_id,
    const char *name,
    const char *state_topic,
    const char *icon)
{
    char topic[128];
    char payload[512];

    snprintf(topic, sizeof(topic),
             "%s/sensor/powerguard/%s/config",
             HA_DISCOVERY_PREFIX, object_id);

    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"unique_id\":\"powerguard_%s_%s\","
             "\"state_topic\":\"%s\","
             "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
             "\"icon\":\"%s\","
             "\"device\":{"
             "\"identifiers\":[\"powerguard_%s\"],"
             "\"name\":\"PowerGuard\","
             "\"manufacturer\":\"DIY\","
             "\"model\":\"ESP32 UPS Controller\""
             "}"
             "}",
             name, s_device_id, object_id, state_topic, icon,
             s_device_id);

    publish(topic, payload, 1, 1);
}

static void publish_ha_discovery_json_sensor(
    const char *object_id,
    const char *name,
    const char *state_topic,
    const char *icon)
{
    char topic[128];
    char payload[768];

    snprintf(topic, sizeof(topic),
             "%s/sensor/powerguard/%s/config",
             HA_DISCOVERY_PREFIX, object_id);

    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"unique_id\":\"powerguard_%s_%s\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.events | length }} events\","
             "\"json_attributes_topic\":\"%s\","
             "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
             "\"icon\":\"%s\","
             "\"device\":{"
             "\"identifiers\":[\"powerguard_%s\"],"
             "\"name\":\"PowerGuard\","
             "\"manufacturer\":\"DIY\","
             "\"model\":\"ESP32 UPS Controller\""
             "}"
             "}",
             name, s_device_id, object_id, state_topic, state_topic, icon,
             s_device_id);

    publish(topic, payload, 1, 1);
}

static void publish_ha_discovery_button(
    const char *object_id,
    const char *name,
    const char *command_topic,
    const char *icon)
{
    char topic[128];
    char payload[512];

    snprintf(topic, sizeof(topic),
             "%s/button/powerguard/%s/config",
             HA_DISCOVERY_PREFIX, object_id);

    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"unique_id\":\"powerguard_%s_%s\","
             "\"command_topic\":\"%s\","
             "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
             "\"icon\":\"%s\","
             "\"device\":{"
             "\"identifiers\":[\"powerguard_%s\"],"
             "\"name\":\"PowerGuard\","
             "\"manufacturer\":\"DIY\","
             "\"model\":\"ESP32 UPS Controller\""
             "}"
             "}",
             name, s_device_id, object_id, command_topic, icon,
             s_device_id);

    publish(topic, payload, 1, 1);
}

static void publish_ha_discovery(void)
{
    publish_ha_discovery_binary_sensor(
        "mains_power", "Mains Power",
        TOPIC_POWER_STATE, "power",
        "online", "lost");

    publish_ha_discovery_binary_sensor(
        "host_online", "Host Online",
        TOPIC_HOST_STATE, "connectivity",
        "up", "down");

    publish_ha_discovery_sensor(
        "shutdown_countdown", "Shutdown Countdown",
        TOPIC_COUNTDOWN, "mdi:timer-alert-outline");

    publish_ha_discovery_sensor(
        "last_event", "Last Event",
        TOPIC_LAST_EVENT, "mdi:message-text-clock-outline");

    publish_ha_discovery_json_sensor(
        "event_log", "Event Log",
        TOPIC_EVENT_LOG, "mdi:math-log");

    publish_ha_discovery_button(
        "cancel_shutdown", "Cancel Shutdown",
        TOPIC_CMD_CANCEL, "mdi:cancel");

    publish_ha_discovery_button(
        "force_wol", "Force Wake-on-LAN",
        TOPIC_CMD_FORCE_WOL, "mdi:power");

    publish_ha_discovery_button(
        "force_shutdown", "Force Shutdown",
        TOPIC_CMD_FORCE_SHUTDOWN, "mdi:power-plug-off");

    publish_ha_discovery_button(
        "clear_log", "Clear Event Log",
        TOPIC_CMD_CLEAR_LOG, "mdi:delete-sweep");

    ESP_LOGI(TAG, "HA discovery payloads published");
}

/* ---------- Command handling ---------- */

static void handle_command(const char *topic, int topic_len)
{
    if (topic_len >= (int)strlen(TOPIC_CMD_CANCEL) &&
        strstr(topic, "cancel_shutdown") != NULL)
    {
        ESP_LOGI(TAG, "Received cancel_shutdown command via MQTT");
        ups_cancel_countdown();
        return;
    }

    if (topic_len >= (int)strlen(TOPIC_CMD_FORCE_WOL) &&
        strstr(topic, "force_wol") != NULL)
    {
        ESP_LOGI(TAG, "Received force_wol command via MQTT");
        wake_on_lan_send(g_ups_config.wake_mac);
        return;
    }

    if (topic_len >= (int)strlen(TOPIC_CMD_FORCE_SHUTDOWN) &&
        strstr(topic, "force_shutdown") != NULL)
    {
        ESP_LOGI(TAG, "Received force_shutdown command via MQTT");
        ups_force_shutdown();
        return;
    }

    if (topic_len >= (int)strlen(TOPIC_CMD_CLEAR_LOG) &&
        strstr(topic, "clear_log") != NULL)
    {
        ESP_LOGI(TAG, "Received clear_log command via MQTT");
        event_log_clear();
        mqtt_publish_event_log();
        return;
    }

    ESP_LOGW(TAG, "Unknown command topic");
}

/* ---------- MQTT event handler ---------- */

static void mqtt_event_handler(void *handler_args,
                                esp_event_base_t base,
                                int32_t event_id,
                                void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");

        publish(TOPIC_AVAILABILITY, "online", 1, 1);

        esp_mqtt_client_subscribe(s_client, TOPIC_CMD_CANCEL, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CMD_FORCE_WOL, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CMD_FORCE_SHUTDOWN, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CMD_CLEAR_LOG, 1);

        publish_ha_discovery();
        ups_publish_current_state();
        mqtt_publish_event_log();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        handle_command(event->topic, event->topic_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error event");
        break;

    default:
        break;
    }
}

/* ---------- Public API ---------- */

void mqtt_manager_init(void)
{
    build_device_id();

    char uri[64];
    snprintf(uri, sizeof(uri), "%s:%lu",
             g_ups_config.mqtt_broker_url,
             (unsigned long)g_ups_config.mqtt_port);

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = g_ups_config.mqtt_username,
        .credentials.authentication.password = g_ups_config.mqtt_password,
        .session.last_will = {
            .topic = TOPIC_AVAILABILITY,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client starting, broker=%s", uri);
}

void mqtt_publish_power_state(const char *state)
{
    publish(TOPIC_POWER_STATE, state, 1, 1);
}

void mqtt_publish_host_state(const char *state)
{
    publish(TOPIC_HOST_STATE, state, 1, 1);
}

void mqtt_publish_countdown(const char *value)
{
    publish(TOPIC_COUNTDOWN, value, 0, 0);
}

void mqtt_publish_last_event(const char *text)
{
    publish(TOPIC_LAST_EVENT, text, 1, 1);
}

void mqtt_publish_event_log(void)
{
    static char log_buf[2048];
    int len = event_log_format_json(log_buf, sizeof(log_buf));
    if (len > 0)
    {
        publish(TOPIC_EVENT_LOG, log_buf, 1, 1);
        ESP_LOGI(TAG, "Event log published (%d bytes)", len);
    }
}
