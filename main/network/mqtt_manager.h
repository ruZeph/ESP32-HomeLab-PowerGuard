#pragma once

void mqtt_manager_init(void);
void mqtt_publish_power_state(const char *state);
void mqtt_publish_host_state(const char *state);
void mqtt_publish_countdown(const char *value);
void mqtt_publish_last_event(const char *text);
void mqtt_publish_event_log(void);
void mqtt_publish_powercut_count(void);
void mqtt_publish_ota_status(const char *state);
