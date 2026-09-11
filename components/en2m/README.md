# `en2m` — ESP-NOW interaction layer

An ESP-IDF component that puts an ESP-NOW capable target on the espnow2mqtt
mesh, either as an end device exposing clusters or as the coordinator bridging
the mesh to the host. The split follows ESP-Matter: the component owns the data
model and the transport, the application owns the hardware.

The same copy of this component is vendored into both
[espnow2mqtt-device](https://github.com/SFNFIH/espnow2mqtt-device) (ESP32-C3
end devices, which use everything below) and
[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host) (the ESP32-S3
coordinator, which uses only the mesh transport and the events).

| Layer | In this component? |
|-------|--------------------|
| Interaction — endpoints, clusters, attributes, commands, reporting | **Yes** (`en2m_model.h`, `en2m_attr.h`) |
| Events | **Yes** (`en2m_event.h`) |
| Transport — ESP-NOW tree, retries | **Yes** (`en2m_mesh.h`) |
| Hardware drivers — GPIO, I2C, PWM, … | **No** — the application reacts to callbacks |

The component runs **one task of its own**. It serves the ESP-NOW receive
queue, mesh maintenance, downlink retries, sensor refreshes and reporting.
An application never polls and, in most cases, never creates a task.

## A complete device

```c
#include "en2m.h"

#define ENDPOINT 1

/* The only place this firmware touches hardware. */
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return relay_set(value->v.b);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "my-sw"},
        .attribute_write = on_write,
    };

    en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

That is the whole firmware. There is no loop, no reporting code and no state
variable: the relay state lives in the attribute store, is persisted to NVS,
is restored into the driver on boot and is reported whenever it changes.

## The two access paths

The distinction matters, and mixing them up is the one thing worth getting
right:

| | Who is the source of truth | What happens |
|---|---|---|
| `en2m_attribute_set` | the application | commit, notify, schedule a report. **Sensors use this.** |
| `en2m_attribute_write` | the network, or local control | run the write callback first, commit only if it returned `ESP_OK`. **Actuators use this.** |

So a PIR calls `en2m_report_occupancy()` (a wrapper around `en2m_attribute_set`),
while a physical light switch calls `en2m_attribute_write()` so the same code
path runs whether the press came from the wall or from Home Assistant.

## Callbacks

All five are optional, all are invoked on the en2m task, and all can be set
device-wide (`en2m_device_config_t`) or per cluster
(`en2m_cluster_set_write_cb`, `..._read_cb`, `..._command_cb`). A per-cluster
handler is tried first; returning `ESP_ERR_NOT_SUPPORTED` falls through to the
device-wide one and then to the built-in behaviour.

| Callback | Purpose |
|---|---|
| `attribute_write` | Apply a value to hardware. A non-OK return means the value is **not** committed and **not** reported. |
| `attribute_read` | Sample a pull-style sensor (I2C, one-wire, ADC) right before a report is built. This is why `th_sensor` has no task. |
| `attribute_changed` | Observe a committed change — logging, a display, local automation. |
| `command` | Implement a command the attribute model cannot express, such as a cover `stop`. |
| `identify` | Blink an LED or beep while the Identify cluster counts down. |

## Events

Everything asynchronous is also published on the ESP-IDF default event loop,
so several parts of an application can observe the stack independently:

```c
static void on_en2m(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == EN2M_EVENT_PARENT_LOST) {
        led_blink_red();
    }
}

en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m, NULL);
```

`EN2M_EVENT_STARTED`, `_STOPPED`, `_PARENT_FOUND`, `_PARENT_LOST`,
`_PAIRING_CHANGED`, `_ATTRIBUTE_UPDATED`, `_COMMAND_RECEIVED`, `_REPORT_SENT`,
`_IDENTIFY`, `_ACK_RECEIVED`, `_ACK_TIMEOUT`, `_RX_DROPPED`.

## Getting out of an ISR

A GPIO interrupt cannot take a mutex or send a packet, so the component lends
the application its task:

```c
static void toggle(void *arg)       /* runs on the en2m task */
{
    en2m_value_t on;
    en2m_attribute_get(1, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &on);
    en2m_attribute_write(1, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!on.v.b));
}

static void button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(toggle, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}
```

`en2m_attribute_set_from_isr()` is the shorthand when all the ISR needs to do
is publish a value.

## Data model

`en2m_endpoint_create_device()` covers the common cases in one call:

```
ON_OFF_LIGHT      DIMMABLE_LIGHT      COLOR_TEMPERATURE_LIGHT
ON_OFF_PLUG       SMART_PLUG          CONTACT_SENSOR
OCCUPANCY_SENSOR  LIGHT_SENSOR        TEMPERATURE_SENSOR
HUMIDITY_SENSOR   PRESSURE_SENSOR     SMOKE_CO_ALARM
FAN               WINDOW_COVERING     DOOR_LOCK
THERMOSTAT
```

Combine several on one endpoint with `en2m_endpoint_add_device_type()`, or
build it up by hand:

```c
en2m_endpoint_t *ep = en2m_endpoint_create(1);
en2m_cluster_t *cl = en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF);  /* adds OnOff attribute */
en2m_attribute_create(cl, 0x4003, en2m_enum8(1), true);             /* StartUpOnOff, persisted */
```

`en2m_cluster_create()` pre-populates the attributes a known cluster is
expected to expose, with Matter-compatible identifiers and defaults.

Supported clusters (a Home-Assistant-oriented Matter subset): Identify, OnOff,
LevelControl, ColorControl (colour temperature), BooleanState, Occupancy,
Illuminance, TemperatureMeasurement, RelativeHumidity, PressureMeasurement,
SmokeCO, DoorLock, WindowCovering, Thermostat, FanControl,
ElectricalPowerMeasurement.

## Reporting

| Mode | Behaviour |
|---|---|
| `EN2M_REPORT_DEFAULT` | On change, rate limited by `min_report_interval_ms` (1 s), plus a keepalive every `report_interval_ms` (30 s leaf, 15 s mains) |
| `EN2M_REPORT_PERIODIC_ONLY` | Only the keepalive |
| `EN2M_REPORT_ON_CHANGE_ONLY` | Only on change |
| `EN2M_REPORT_MANUAL` | Only `en2m_report_now()` |

A report is a flat JSON object (`switch`, `temperature`, `brightness`, …) plus
a `caps` list, which is what the MQTT bridge and the Home Assistant
integration consume. An ESP-NOW frame carries at most `EN2M_DATA_MAX` (160)
payload bytes, so the component drops the optional fields — diagnostics first,
then `caps` — rather than emitting a truncated object. The host merges
successive reports, so a partial report is never lossy.

## Reliability

`en2m_send_downlink()` with a non-zero transaction id is retried
(`EN2M_CMD_RETRIES`, every `EN2M_CMD_RETRY_MS`) until the device acknowledges
it. Success and failure are published as `EN2M_EVENT_ACK_RECEIVED` and
`EN2M_EVENT_ACK_TIMEOUT`; the coordinator turns the latter into an
`{"ok": false, "error": "timeout"}` line for the host.

Devices acknowledge automatically: the interaction layer replies to every CMD
frame that carries a transaction id, then reports the resulting state.

## Configuration

`idf.py menuconfig` → *ESP-NOW Mesh (en2m)*: channel, hop limit, route and
neighbour table sizes, beacon and heartbeat intervals, queue depth, retry
counts, and the data model limits (endpoints, clusters per endpoint,
attributes per cluster).

## Migrating from the driver-ops API

Releases before 0.4 asked the application to fill in
`en2m_on_off_driver_t`-style structs and to call `en2m_model_loop()` from its
own loop. Those structs are gone:

| Before | Now |
|---|---|
| `en2m_endpoint_add_on_off(ep, &drv)` | `en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF)` plus an `attribute_write` callback |
| `driver.set(bool on, ctx)` | `attribute_write` on `EN2M_CLUSTER_ON_OFF` |
| `driver.get(bool *on, ctx)` | not needed — the component caches state; use `attribute_read` only for live sensors |
| `en2m_model_start(&mesh)` | `en2m_start(&device_config)` |
| `en2m_model_loop()` / `en2m_mesh_loop()` in a `while` loop | delete it; both are no-ops now |
| `en2m_model_notify(ep, cluster, true)` | `en2m_attribute_set()`, which reports by itself |

See the repository root README for the wiring and the ten worked examples.
