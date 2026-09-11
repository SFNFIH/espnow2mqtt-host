/**
 * @file en2m_model.h
 * @brief Interaction / data-model layer (ESP-Matter style).
 *
 * This layer owns endpoints, clusters, attributes and commands.
 * It does **not** own hardware drivers — the application binds drivers
 * through ops callbacks (similar to ESP-Matter's driver glue).
 *
 * Layers:
 *   Application + drivers  →  en2m model (clusters)  →  en2m mesh (ESP-NOW)
 *
 * Cluster IDs follow Matter / Zigbee where practical. Cap names map to
 * Home Assistant MQTT entity platforms (light, fan, cover, lock, climate, …).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "en2m_mesh.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Matter-inspired cluster identifiers (HA-oriented subset). */
typedef enum {
    EN2M_CLUSTER_ON_OFF = 0x0006,
    EN2M_CLUSTER_LEVEL_CONTROL = 0x0008,
    EN2M_CLUSTER_DOOR_LOCK = 0x0101,
    EN2M_CLUSTER_WINDOW_COVERING = 0x0102,
    EN2M_CLUSTER_THERMOSTAT = 0x0201,
    EN2M_CLUSTER_FAN_CONTROL = 0x0202,
    EN2M_CLUSTER_COLOR_CONTROL = 0x0300,
    EN2M_CLUSTER_ILLUMINANCE = 0x0400,
    EN2M_CLUSTER_TEMPERATURE_MEASUREMENT = 0x0402,
    EN2M_CLUSTER_PRESSURE_MEASUREMENT = 0x0403,
    EN2M_CLUSTER_RELATIVE_HUMIDITY = 0x0405,
    EN2M_CLUSTER_OCCUPANCY = 0x0406,
    EN2M_CLUSTER_BOOLEAN_STATE = 0x0045,
    EN2M_CLUSTER_SMOKE_CO = 0x005C,
    EN2M_CLUSTER_ELECTRICAL_POWER = 0x0B04,
} en2m_cluster_id_t;

/** Thermostat system mode (Matter-like subset). */
typedef enum {
    EN2M_THERMOSTAT_OFF = 0,
    EN2M_THERMOSTAT_AUTO = 1,
    EN2M_THERMOSTAT_COOL = 3,
    EN2M_THERMOSTAT_HEAT = 4,
    EN2M_THERMOSTAT_FAN_ONLY = 7,
} en2m_thermostat_mode_t;

/** Fan mode. */
typedef enum {
    EN2M_FAN_OFF = 0,
    EN2M_FAN_LOW = 1,
    EN2M_FAN_MEDIUM = 2,
    EN2M_FAN_HIGH = 3,
    EN2M_FAN_ON = 4,
    EN2M_FAN_AUTO = 5,
    EN2M_FAN_SMART = 6,
} en2m_fan_mode_t;

/** Window covering lift percent (0 = open / fully retracted, 100 = closed). */
typedef enum {
    EN2M_COVER_STOP = 0,
    EN2M_COVER_OPEN = 1,
    EN2M_COVER_CLOSE = 2,
    EN2M_COVER_GOTO = 3,
} en2m_cover_command_t;

#define EN2M_MAX_ENDPOINTS 4

typedef struct en2m_endpoint en2m_endpoint_t;

/* ---- Driver ops (interaction ← application) ---- */

typedef struct {
    esp_err_t (*set)(bool on, void *ctx);
    esp_err_t (*get)(bool *on, void *ctx);
    void *ctx;
} en2m_on_off_driver_t;

typedef struct {
    /** level 0–254 (Matter-like); 255 = null / unused */
    esp_err_t (*set_level)(uint8_t level, void *ctx);
    esp_err_t (*get_level)(uint8_t *level, void *ctx);
    void *ctx;
} en2m_level_driver_t;

typedef struct {
    /** Color temperature in mireds (Matter ColorControl). */
    esp_err_t (*set_color_temp)(uint16_t mireds, void *ctx);
    esp_err_t (*get_color_temp)(uint16_t *mireds, void *ctx);
    void *ctx;
} en2m_color_control_driver_t;

typedef struct {
    esp_err_t (*get)(bool *state_value, void *ctx);
    void *ctx;
} en2m_boolean_state_driver_t;

typedef struct {
    esp_err_t (*get_occupied)(bool *occupied, void *ctx);
    void *ctx;
} en2m_occupancy_driver_t;

typedef struct {
    /** Lux as Matter MeasuredValue (10000 * log10(lux) + 1), or raw lux via get_lux. */
    esp_err_t (*get_lux)(uint32_t *lux, void *ctx);
    void *ctx;
} en2m_illuminance_driver_t;

typedef struct {
    /** Return temperature in 0.01 °C (e.g. 2350 = 23.50 °C), Matter-style. */
    esp_err_t (*get_measured_value)(int16_t *centi_celsius, void *ctx);
    void *ctx;
} en2m_temperature_driver_t;

typedef struct {
    /** Return humidity in 0.01 % (e.g. 5510 = 55.10 %). */
    esp_err_t (*get_measured_value)(uint16_t *centi_percent, void *ctx);
    void *ctx;
} en2m_humidity_driver_t;

typedef struct {
    /** Pressure in 0.1 Pa Matter units, or hPa via get_hpa. */
    esp_err_t (*get_hpa)(int32_t *hpa_x10, void *ctx); /* 0.1 hPa */
    void *ctx;
} en2m_pressure_driver_t;

typedef struct {
    esp_err_t (*get_active_power)(int32_t *milliwatts, void *ctx);
    esp_err_t (*get_energy)(int64_t *milliwatt_hours, void *ctx);
    void *ctx;
} en2m_electrical_power_driver_t;

typedef struct {
    esp_err_t (*set_mode)(en2m_fan_mode_t mode, void *ctx);
    esp_err_t (*get_mode)(en2m_fan_mode_t *mode, void *ctx);
    /** Optional 0–100 percent; NULL if unsupported. */
    esp_err_t (*set_percent)(uint8_t percent, void *ctx);
    esp_err_t (*get_percent)(uint8_t *percent, void *ctx);
    void *ctx;
} en2m_fan_control_driver_t;

typedef struct {
    esp_err_t (*command)(en2m_cover_command_t cmd, uint8_t position, void *ctx);
    /** position 0=open … 100=closed */
    esp_err_t (*get_position)(uint8_t *position, void *ctx);
    void *ctx;
} en2m_window_covering_driver_t;

typedef struct {
    esp_err_t (*lock)(void *ctx);
    esp_err_t (*unlock)(void *ctx);
    esp_err_t (*get_locked)(bool *locked, void *ctx);
    void *ctx;
} en2m_door_lock_driver_t;

typedef struct {
    esp_err_t (*set_system_mode)(en2m_thermostat_mode_t mode, void *ctx);
    esp_err_t (*get_system_mode)(en2m_thermostat_mode_t *mode, void *ctx);
    /** Setpoints in 0.01 °C. */
    esp_err_t (*set_occupied_heating)(int16_t centi_celsius, void *ctx);
    esp_err_t (*get_occupied_heating)(int16_t *centi_celsius, void *ctx);
    esp_err_t (*set_occupied_cooling)(int16_t centi_celsius, void *ctx);
    esp_err_t (*get_occupied_cooling)(int16_t *centi_celsius, void *ctx);
    /** Optional local temperature sensor on same cluster. */
    esp_err_t (*get_local_temperature)(int16_t *centi_celsius, void *ctx);
    void *ctx;
} en2m_thermostat_driver_t;

typedef struct {
    esp_err_t (*get_smoke)(bool *alarm, void *ctx);
    esp_err_t (*get_co)(bool *alarm, void *ctx);
    void *ctx;
} en2m_smoke_co_driver_t;

/**
 * @brief Create an endpoint (1..254). Call before ::en2m_model_start.
 */
en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id);

esp_err_t en2m_endpoint_add_on_off(en2m_endpoint_t *ep, const en2m_on_off_driver_t *driver);
esp_err_t en2m_endpoint_add_level_control(en2m_endpoint_t *ep, const en2m_level_driver_t *driver);
esp_err_t en2m_endpoint_add_color_control(en2m_endpoint_t *ep, const en2m_color_control_driver_t *driver);
esp_err_t en2m_endpoint_add_boolean_state(en2m_endpoint_t *ep, const en2m_boolean_state_driver_t *driver);
esp_err_t en2m_endpoint_add_occupancy(en2m_endpoint_t *ep, const en2m_occupancy_driver_t *driver);
esp_err_t en2m_endpoint_add_illuminance(en2m_endpoint_t *ep, const en2m_illuminance_driver_t *driver);
esp_err_t en2m_endpoint_add_temperature(en2m_endpoint_t *ep, const en2m_temperature_driver_t *driver);
esp_err_t en2m_endpoint_add_humidity(en2m_endpoint_t *ep, const en2m_humidity_driver_t *driver);
esp_err_t en2m_endpoint_add_pressure(en2m_endpoint_t *ep, const en2m_pressure_driver_t *driver);
esp_err_t en2m_endpoint_add_electrical_power(en2m_endpoint_t *ep, const en2m_electrical_power_driver_t *driver);
esp_err_t en2m_endpoint_add_fan_control(en2m_endpoint_t *ep, const en2m_fan_control_driver_t *driver);
esp_err_t en2m_endpoint_add_window_covering(en2m_endpoint_t *ep, const en2m_window_covering_driver_t *driver);
esp_err_t en2m_endpoint_add_door_lock(en2m_endpoint_t *ep, const en2m_door_lock_driver_t *driver);
esp_err_t en2m_endpoint_add_thermostat(en2m_endpoint_t *ep, const en2m_thermostat_driver_t *driver);
esp_err_t en2m_endpoint_add_smoke_co(en2m_endpoint_t *ep, const en2m_smoke_co_driver_t *driver);

/**
 * @brief Init mesh transport and mark the data model ready.
 *
 * @note Does not touch GPIO / I2C / etc. Drivers must already be bound.
 */
esp_err_t en2m_model_start(const en2m_config_t *mesh_config);

/** Periodic: mesh maintenance + optional auto-report. */
void en2m_model_loop(void);

/** Force an attribute report uplink now. */
esp_err_t en2m_model_report(void);

/**
 * @brief Notify model that a measured attribute changed (e.g. contact toggled).
 * Triggers a report on next loop or immediately if @p immediate.
 */
esp_err_t en2m_model_notify(uint8_t endpoint_id, en2m_cluster_id_t cluster, bool immediate);

#ifdef __cplusplus
}
#endif
