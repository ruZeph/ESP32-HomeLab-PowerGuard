# UPS Controller ESP32

ESP32-based power-loss controller for a homelab/server host.

When AC power disappears, the controller starts a shutdown countdown. If the outage lasts long enough, it calls a host shutdown API, waits until the host is actually offline, and then stays quiet until power returns. When mains power comes back, it sends a Wake-on-LAN packet and waits for the host health check to recover. All state transitions are published to an MQTT broker for Home Assistant integration.

## Features

- Detects external power presence with a dedicated GPIO input
- Shows power state on red/green status LEDs
- Connects to Wi-Fi in STA mode and auto-retries on disconnect
- Delays shutdown to avoid reacting to short outages
- Calls an authenticated HTTP shutdown endpoint
- Confirms the host is really down before considering shutdown complete
- Sends Wake-on-LAN only after shutdown is confirmed, with up to 20 automatic retries
- Polls a host health endpoint after wake until the server is back online
- Suppresses repeated noisy logs once shutdown has already been confirmed
- Maintains a persistent 30-event ring buffer in NVS flash to survive sudden outages
- Reports state to Home Assistant via MQTT with auto-discovery
- Accepts remote commands (cancel shutdown, force WOL, force shutdown, clear log) over MQTT
- Publishes live shutdown countdown over MQTT
- Dumps full event log timeline to Home Assistant automatically on reconnect
- Uses MQTT Last Will and Testament for ESP32 availability tracking

## Current Host Flow

This project is currently configured for a host on `192.168.0.89`:

- Shutdown API: `POST http://192.168.0.89:8787/api/v1/host/poweroff`
- Health check: `GET http://192.168.0.89:8787/health`
- Wake target MAC: `AA:BB:CC:DD:EE:FF`

Expected shutdown API behavior:

- Requires `Authorization: Bearer <token>`
- Returns `202 Accepted` when shutdown has been accepted

Expected host-side shutdown action:

```sh
docker stop $(docker ps -q) && sudo shutdown -h
```

## How It Works

Normal sequence:

1. ESP32 boots and connects to Wi-Fi.
2. GPIO power input is read periodically.
3. If power is lost, a shutdown countdown starts.
4. If power comes back before the countdown expires, shutdown is canceled.
5. If the countdown expires, the controller calls the shutdown API.
6. The controller polls the health endpoint until the host stops responding.
7. Once the host is confirmed down, polling stops while power is still out.
8. When power returns, the controller sends a Wake-on-LAN packet.
9. The controller polls the health endpoint until the host is reachable again.
10. The controller returns to idle.

Important behavior detail:

- If power returns while the host is still shutting down, the ESP32 waits for shutdown confirmation before sending Wake-on-LAN. This avoids racing the host during power restoration.

## MQTT / Home Assistant

The controller connects to an MQTT broker and publishes state on every transition. Home Assistant auto-discovery payloads are sent on each MQTT connect, creating entities automatically.

### Published Topics

| Topic | Payload | Notes |
|---|---|---|
| `powerguard/power_state` | `"online"` / `"lost"` | Retained |
| `powerguard/host_state` | `"up"` / `"down"` | Retained |
| `powerguard/countdown` | Seconds remaining or `"idle"` | Updated every poll cycle during countdown |
| `powerguard/last_event` | Free text, e.g. `"shutdown cancelled"` | Retained |
| `powerguard/event_log` | JSON array of last 30 events | Retained, automatically published on reconnect |
| `powerguard/availability` | `"online"` / `"offline"` | LWT topic, retained. `"offline"` set by broker if ESP32 disconnects |

The `availability` topic is the MQTT Last Will and Testament. Home Assistant uses it to distinguish "ESP32 itself is offline" from "mains power is lost" — these are different failure states.

### Subscribed Topics (Commands)

| Topic | Effect |
|---|---|
| `powerguard/command/cancel_shutdown` | Cancels an in-progress shutdown countdown (same as power restoration) |
| `powerguard/command/force_wol` | Sends an immediate Wake-on-LAN packet regardless of controller state |
| `powerguard/command/force_shutdown`| Instantly aborts countdown and sends shutdown API request |
| `powerguard/command/clear_log` | Erases the NVS flash event ring buffer and clears dashboard table |

### Home Assistant Discovery

On connect, retained discovery config payloads are published for these entities:

| Entity | HA Component | Device Class / Icon |
|---|---|---|
| Mains Power | `binary_sensor` | `power` |
| Host Online | `binary_sensor` | `connectivity` |
| Shutdown Countdown | `sensor` | `mdi:timer-alert-outline` |
| Last Event | `sensor` | `mdi:message-text-clock-outline` |
| Event Log | `sensor` | `mdi:format-list-bulleted-type` |
| Cancel Shutdown | `button` | `mdi:cancel` |
| Force Wake-on-LAN | `button` | `mdi:power` |
| Force Shutdown | `button` | `mdi:power-plug-off` |
| Clear Event Log | `button` | `mdi:delete-sweep` |

All entities are grouped under a single "PowerGuard" device in the HA UI, identified by the ESP32's MAC address.

### Event Log Dashboard Card

To visualize the persistent event log beautifully in Home Assistant, create a Markdown Card in your Lovelace dashboard and use the following Jinja template. It will automatically parse the `event_log` JSON, map events to nice emojis, and instantly auto-refresh whenever a new event occurs:

```markdown
<details open>
<summary><b>Latest 30 Events (Click to collapse/expand)</b></summary>

| ⏱️ Uptime | 📜 Event | ℹ️ Details |
| :--- | :--- | :--- |
{%- set events = state_attr('sensor.powerguard_event_log', 'events') %}
{%- if events %}
{%- for evt in events | reverse %}
  {%- set type_map = {
    'ESP_BOOTED': '🟢 System Boot',
    'POWER_LOST': '🔴 Power Lost',
    'POWER_RESTORED': '⚡ Power Restored',
    'SHUTDOWN_TRIGGERED': '💀 Shutdown Triggered',
    'SHUTDOWN_FAILED': '❌ Shutdown Failed',
    'SHUTDOWN_CANCELLED': '🛑 Shutdown Cancelled',
    'HOST_DOWN_CONFIRMED': '📴 Host Offline',
    'HOST_BACK_ONLINE': '🚀 Host Back Online',
    'WOL_SENT': '📡 WOL Sent',
    'WOL_RETRY': '🔄 WOL Retry',
    'WOL_FAILED': '⚠️ WOL Failed'
  } %}
  {%- set evt_name = type_map.get(evt.e, evt.e) %}
  {%- set h = (evt.t / 3600) | int %}
  {%- set m = ((evt.t % 3600) / 60) | int %}
  {%- set s = evt.t % 60 %}
  {%- set t_str = "%02d:%02d:%02d" | format(h, m, s) %}
| `{{ t_str }}` | **{{ evt_name }}** | {{ evt.x | default('-') }} |
{%- endfor %}
{%- else %}
| - | No events recorded | - |
{%- endif %}

</details>
<span style="display:none">{{ states('sensor.powerguard_last_event') }}</span>
```

## Hardware

Current GPIO usage:

- `GPIO34`: power sense input
- `GPIO25`: green LED, power present
- `GPIO26`: red LED, power lost
- `GPIO2`: onboard Wi-Fi status LED

Power sensing logic:

- HIGH on `GPIO34` means power present
- LOW on `GPIO34` means power lost

## Board Wiring

Actual perfboard wiring reference:

![ESP32 UPS controller board wiring](./assets/1782849502688.png)

This board is a compact ESP32 power-cut sensor built on a `6 cm x 8 cm` perfboard. The ESP32 reads a divided version of the USB `+5V` rail on `GPIO34` to decide whether mains-backed power is present, and it drives two status LEDs directly from GPIO pins.

Connection summary:

- `GPIO34`: divider sense input from the USB `+5V` line
- `GPIO25`: green LED output
- `GPIO26`: red LED output
- `GND`: shared/common ground
- `USB +5V`: source used for the divider input

Divider network shown in the board image:

- `1k` from ESP32-side sense trace into the divider node
- `10k` from divider node to USB `+5V`
- `15k` from divider node to `GND`

LED wiring shown in the board image:

- Green LED anode is driven from `GPIO25` through `220 ohm`
- Red LED anode is driven from `GPIO26` through `220 ohm`
- Both LED cathodes return to common ground

Important notes for this build:

- The LEDs are software-controlled and are not tied directly to the `+5V` rail
- The `GPIO34` trace comes from the divider output, not directly from `+5V`
- The divider output is approximately `3.0V` when USB `+5V` is present
- `GPIO34` is input-only on ESP32, which makes it suitable for power sensing

## Project Layout

```text
main/
  app/
    main.c
    ups.c
    ups.h
  config/
    config.h
    config.c        # local, ignored by git
  drivers/
    leds.c
    leds.h
    power_sensor.c
    power_sensor.h
  network/
    mqtt_manager.c
    mqtt_manager.h
    shutdown_client.c
    shutdown_client.h
    wake_on_lan.c
    wake_on_lan.h
    wifi_manager.c
    wifi_manager.h
  idf_component.yml  # managed component dependency (espressif/mqtt)
```

## Configuration

Configuration lives in:

- `main/config/config.h`
- `main/config/config.c`

`main/config/config.c` is intentionally ignored by git so Wi-Fi credentials and bearer tokens stay local.

Example configuration:

```c
#include "config.h"

const ups_config_t g_ups_config = {
    .power_poll_interval_ms = 5000,
    .shutdown_delay_seconds = 30,
    .health_check_timeout_ms = 20000,
    .wifi_ssid = "YOUR_WIFI_SSID",
    .wifi_password = "YOUR_WIFI_PASSWORD",
    .wake_mac = "AA:BB:CC:DD:EE:FF",
    .shutdown_url = "http://192.168.0.89:8787/api/v1/host/poweroff",
    .shutdown_bearer_token = "YOUR_TOKEN",
    .healthcheck_url = "http://192.168.0.89:8787/health",
    .mqtt_broker_url = "mqtt://192.168.0.89",
    .mqtt_port = 1883,
    .mqtt_username = "esp32",
    .mqtt_password = "YOUR_MQTT_PASSWORD",
};
```

Configuration fields:

- `power_poll_interval_ms`: interval between power-state checks
- `shutdown_delay_seconds`: how long power must stay out before shutdown is triggered
- `health_check_timeout_ms`: timeout used for health-check HTTP requests
- `wifi_ssid`: Wi-Fi network name
- `wifi_password`: Wi-Fi password
- `wake_mac`: MAC address of the host to wake
- `shutdown_url`: host shutdown endpoint
- `shutdown_bearer_token`: bearer token for shutdown authentication
- `healthcheck_url`: host endpoint used to confirm down/up status
- `mqtt_broker_url`: MQTT broker URI, e.g. `mqtt://192.168.0.89`
- `mqtt_port`: MQTT broker port, typically `1883`
- `mqtt_username`: MQTT username
- `mqtt_password`: MQTT password (keep in gitignored `config.c`)

## Build and Flash

Standard ESP-IDF workflow:

```sh
idf.py set-target esp32
idf.py build
idf.py -p COM3 flash monitor
```

If target is already set, a normal cycle is usually:

```sh
idf.py build flash monitor
```

## Runtime Logs

Typical useful logs:

- `UPS: Power LOST`
- `UPS: Shutdown countdown started (30 s)`
- `UPS: Shutdown cancelled`
- `SHUTDOWN: Shutdown request accepted, HTTP status = 202`
- `UPS: Host shutdown confirmed`
- `UPS: Host is off, waiting for power restoration before Wake-on-LAN`
- `WOL: WOL packet sent to ...`
- `UPS: Host is back online`
- `MQTT: Connected to MQTT broker`
- `MQTT: HA discovery payloads published`
- `MQTT: Received cancel_shutdown command via MQTT`
- `MQTT: Received force_wol command via MQTT`
- `MQTT: Disconnected from MQTT broker`

Expected temporary errors during shutdown or boot:

- `ESP_ERR_HTTP_CONNECT`
- `Connection reset by peer`
- `select() timeout`

Those usually mean the host is shutting down or the health service is not up yet. The controller now treats those as expected during transition states.

## Host Requirements

For reliable operation, the server should have:

- A shutdown API that accepts authenticated POST requests
- A health endpoint that is available when the host is fully up
- Wake-on-LAN enabled in BIOS/UEFI
- Wake-on-LAN enabled in the OS and NIC driver
- UDP broadcast on port `9` allowed on the local network
- An MQTT broker reachable on the LAN (e.g. Mosquitto on the Home Assistant host)

## Troubleshooting

If shutdown works but wake does not:

- Verify BIOS/UEFI Wake-on-LAN settings
- Verify the NIC supports wake from soft-off state
- Verify the MAC address in `wake_mac`
- Check that the switch/router path allows LAN broadcast traffic

If wake happens but the controller keeps waiting:

- Test `curl http://<host>:8787/health` from another LAN machine
- Confirm the health service starts automatically after boot
- Confirm local firewall rules allow the ESP32 to reach port `8787`

If short outages trigger shutdown too aggressively:

- Increase `shutdown_delay_seconds`

If recovery feels slow:

- Lower `power_poll_interval_ms`
- Lower `health_check_timeout_ms`

## Notes

- This project currently uses a small native ESP-IDF Wake-on-LAN implementation in `main/network/wake_on_lan.c`.
- The previously considered Arduino-oriented `a7md0/WakeOnLan` library was not integrated because this project is plain ESP-IDF, not Arduino.
- Sensitive values should stay only in the ignored local config file.
- MQTT uses the `espressif/mqtt` managed component, declared in `main/idf_component.yml`.

If MQTT entities don't appear in Home Assistant:

- Verify the MQTT broker is running and reachable from the ESP32's subnet
- Verify `mqtt_username` and `mqtt_password` in `config.c`
- Check that Home Assistant's MQTT integration has discovery enabled (it is by default)
- Check serial logs for `MQTT: Connected to MQTT broker` and `MQTT: HA discovery payloads published`

If MQTT commands don't work:

- Verify subscription logs: `MQTT: Subscribed, msg_id=...`
- Publish manually: `mosquitto_pub -t powerguard/command/force_wol -m test`
- Check that the ESP32 is connected (availability topic should show `"online"`)

## License

Add the license you want for this repository.
