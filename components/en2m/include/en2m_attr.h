/**
 * @file en2m_attr.h
 * @brief Data types of the interaction layer: values, identifiers, attributes, commands.
 *
 * The component owns device state. Applications never poll it:
 *
 *  - a sensor **pushes** a new reading in with ::en2m_attribute_set
 *  - an actuator **receives** writes through an ::en2m_attribute_write_cb_t
 *  - a pull-style sensor exposes an ::en2m_attribute_read_cb_t and the
 *    component reads it just before every report
 *
 * Cluster and attribute identifiers follow Matter wherever a matching
 * concept exists, so the model maps cleanly onto both Matter and the
 * Home Assistant MQTT platforms.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Matter-inspired cluster identifiers (HA-oriented subset). */
typedef enum {
    EN2M_CLUSTER_IDENTIFY = 0x0003,
    EN2M_CLUSTER_ON_OFF = 0x0006,
    EN2M_CLUSTER_LEVEL_CONTROL = 0x0008,
    EN2M_CLUSTER_SWITCH = 0x003B,
    EN2M_CLUSTER_BOOLEAN_STATE = 0x0045,
    EN2M_CLUSTER_SMOKE_CO = 0x005C,
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
    EN2M_CLUSTER_ELECTRICAL_POWER = 0x0B04,
} en2m_cluster_id_t;

/** Attribute identifiers, scoped per cluster. */
typedef enum {
    /* Identify */
    EN2M_ATTR_IDENTIFY_TIME = 0x0000,
    /* OnOff */
    EN2M_ATTR_ON_OFF = 0x0000,
    /* LevelControl: 0–254 */
    EN2M_ATTR_CURRENT_LEVEL = 0x0000,
    /* Switch */
    EN2M_ATTR_NUMBER_OF_POSITIONS = 0x0000,
    EN2M_ATTR_CURRENT_POSITION = 0x0001,
    EN2M_ATTR_MULTI_PRESS_MAX = 0x0002,
    /* Switch, transport-specific. Matter delivers presses as events, which
     * this protocol has no equivalent for: every uplink is a full state
     * snapshot and a retained one at that. A press is therefore carried as a
     * monotonically increasing counter plus the kind of the last press, so a
     * receiver can tell a new press from a resend of the previous report. See
     * ::en2m_report_button. */
    EN2M_ATTR_PRESS_COUNT = 0xFF00,
    EN2M_ATTR_PRESS_ACTION = 0xFF01,
    /* ColorControl */
    EN2M_ATTR_COLOR_TEMPERATURE_MIREDS = 0x0007,
    /* BooleanState / Occupancy / measurement clusters */
    EN2M_ATTR_STATE_VALUE = 0x0000,
    EN2M_ATTR_OCCUPANCY = 0x0000,
    EN2M_ATTR_MEASURED_VALUE = 0x0000,
    /* SmokeCO */
    EN2M_ATTR_SMOKE_STATE = 0x0001,
    EN2M_ATTR_CO_STATE = 0x0002,
    /* DoorLock */
    EN2M_ATTR_LOCK_STATE = 0x0000,
    /* WindowCovering: 0 = fully open, 100 = fully closed */
    EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT = 0x0008,
    /* Thermostat, in 0.01 degC */
    EN2M_ATTR_LOCAL_TEMPERATURE = 0x0000,
    EN2M_ATTR_OCCUPIED_COOLING_SETPOINT = 0x0011,
    EN2M_ATTR_OCCUPIED_HEATING_SETPOINT = 0x0012,
    EN2M_ATTR_SYSTEM_MODE = 0x001C,
    /* FanControl */
    EN2M_ATTR_FAN_MODE = 0x0000,
    EN2M_ATTR_PERCENT_SETTING = 0x0002,
    /* ElectricalPowerMeasurement */
    EN2M_ATTR_ACTIVE_POWER_MW = 0x000A,
    EN2M_ATTR_ENERGY_MWH = 0x0011,
} en2m_attribute_id_t;

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

/** Door lock state. */
typedef enum {
    EN2M_LOCK_UNLOCKED = 0,
    EN2M_LOCK_LOCKED = 1,
} en2m_lock_state_t;

/**
 * @brief Kind of button press, as the Home Assistant event platform names them.
 *
 * These map onto Matter's Switch events — InitialPress/ShortRelease,
 * MultiPressComplete with two presses, LongPress, LongRelease — collapsed to
 * the four an automation actually branches on.
 */
typedef enum {
    EN2M_PRESS_SHORT = 0,  /**< A single short press: "press" */
    EN2M_PRESS_DOUBLE = 1, /**< Two presses in quick succession: "double_press" */
    EN2M_PRESS_LONG = 2,   /**< Held past the long-press threshold: "long_press" */
    EN2M_PRESS_RELEASE = 3,/**< Let go after a long press: "release" */
} en2m_press_action_t;

/** Value discriminator. */
typedef enum {
    EN2M_VAL_NULL = 0,
    EN2M_VAL_BOOL,
    EN2M_VAL_U8,
    EN2M_VAL_U16,
    EN2M_VAL_U32,
    EN2M_VAL_I16,
    EN2M_VAL_I32,
    EN2M_VAL_I64,
    EN2M_VAL_ENUM8,
} en2m_val_type_t;

/** Tagged attribute value. */
typedef struct {
    en2m_val_type_t type;
    union {
        bool b;
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        int16_t i16;
        int32_t i32;
        int64_t i64;
        uint8_t e8;
    } v;
} en2m_value_t;

/** Fully qualified attribute path. */
typedef struct {
    uint8_t endpoint_id;
    uint16_t cluster_id;
    uint16_t attribute_id;
} en2m_attr_path_t;

/* ---- Value constructors ---- */

static inline en2m_value_t en2m_bool(bool value)
{
    en2m_value_t out = {.type = EN2M_VAL_BOOL};
    out.v.b = value;
    return out;
}

static inline en2m_value_t en2m_u8(uint8_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_U8};
    out.v.u8 = value;
    return out;
}

static inline en2m_value_t en2m_u16(uint16_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_U16};
    out.v.u16 = value;
    return out;
}

static inline en2m_value_t en2m_u32(uint32_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_U32};
    out.v.u32 = value;
    return out;
}

static inline en2m_value_t en2m_i16(int16_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_I16};
    out.v.i16 = value;
    return out;
}

static inline en2m_value_t en2m_i32(int32_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_I32};
    out.v.i32 = value;
    return out;
}

static inline en2m_value_t en2m_i64(int64_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_I64};
    out.v.i64 = value;
    return out;
}

static inline en2m_value_t en2m_enum8(uint8_t value)
{
    en2m_value_t out = {.type = EN2M_VAL_ENUM8};
    out.v.e8 = value;
    return out;
}

/** Best-effort numeric view of any value (bools become 0/1, NULL becomes 0). */
int64_t en2m_value_as_int(const en2m_value_t *value);

/** True when both values carry the same type and payload. */
bool en2m_value_equal(const en2m_value_t *a, const en2m_value_t *b);

/* ---- Commands ---- */

/** Decoded command identifiers (Matter command names where they exist). */
typedef enum {
    EN2M_CMD_UNKNOWN = 0,
    EN2M_CMD_OFF,
    EN2M_CMD_ON,
    EN2M_CMD_TOGGLE,
    EN2M_CMD_MOVE_TO_LEVEL,
    EN2M_CMD_MOVE_TO_COLOR_TEMPERATURE,
    EN2M_CMD_LOCK_DOOR,
    EN2M_CMD_UNLOCK_DOOR,
    EN2M_CMD_UP_OR_OPEN,
    EN2M_CMD_DOWN_OR_CLOSE,
    EN2M_CMD_STOP_MOTION,
    EN2M_CMD_GO_TO_LIFT_PERCENTAGE,
    EN2M_CMD_SET_FAN_MODE,
    EN2M_CMD_SET_FAN_PERCENT,
    EN2M_CMD_SET_SYSTEM_MODE,
    EN2M_CMD_SET_HEATING_SETPOINT,
    EN2M_CMD_SET_COOLING_SETPOINT,
    EN2M_CMD_IDENTIFY,
    EN2M_CMD_WRITE_ATTRIBUTE,
} en2m_command_id_t;

/** A command decoded from a downlink frame. */
typedef struct {
    uint8_t endpoint_id;      /**< Target endpoint */
    uint16_t cluster_id;      /**< Target cluster */
    en2m_command_id_t id;     /**< Decoded command */
    const char *name;         /**< Command name as received, never NULL */
    en2m_value_t arg;         /**< Primary argument, ::EN2M_VAL_NULL when absent */
    uint16_t transaction_id;  /**< Frame cmd_id, echoed in the ACK */
    const char *json;         /**< Raw payload, for application-specific fields */
} en2m_command_t;

/* ---- Application callbacks ---- */

/**
 * @brief Apply a write to hardware.
 *
 * Invoked from the en2m task for every remote command and every
 * ::en2m_attribute_write. The new value is committed to the attribute store
 * and reported only when this returns ESP_OK, so a failing actuator never
 * reports a state it did not reach.
 *
 * Return ESP_ERR_NOT_SUPPORTED to let the component fall through to the next
 * handler (cluster handler → device handler → plain commit).
 */
typedef esp_err_t (*en2m_attribute_write_cb_t)(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx);

/**
 * @brief Read a live value from hardware.
 *
 * Invoked from the en2m task just before a report is built, which is how
 * pull-style sensors (I2C, one-wire, ADC) stay poll-free in the application.
 * Return ESP_ERR_NOT_SUPPORTED to keep the cached value.
 */
typedef esp_err_t (*en2m_attribute_read_cb_t)(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx);

/** @brief Observe a committed change (logging, local automation, displays). */
typedef void (*en2m_attribute_changed_cb_t)(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx);

/**
 * @brief Handle a command the application wants to implement itself.
 *
 * Runs before the built-in translation to attribute writes. Return ESP_OK to
 * consume the command, or ESP_ERR_NOT_SUPPORTED to let the component handle it.
 */
typedef esp_err_t (*en2m_command_handler_t)(const en2m_command_t *command, void *ctx);

/** @brief Drive a visual/audible identify effect for @p seconds. */
typedef void (*en2m_identify_cb_t)(uint8_t endpoint_id, uint16_t seconds, void *ctx);

/* ---- Attribute access (thread-safe, callable from any task) ---- */

/**
 * @brief Publish a new value measured by the application.
 *
 * This is the sensor path: it commits the value, notifies observers and
 * schedules a report. It deliberately does **not** invoke the write callback,
 * because the application is already the source of truth.
 */
esp_err_t en2m_attribute_set(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                             en2m_value_t value);

/** @brief ISR-safe ::en2m_attribute_set; the commit happens in the en2m task. */
esp_err_t en2m_attribute_set_from_isr(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                                      en2m_value_t value, BaseType_t *higher_prio_task_woken);

/**
 * @brief Drive an attribute through the write path, as a remote command would.
 *
 * Use this for local control (a physical button, a scene, a timer): the write
 * callback runs, hardware moves, and the state is reported once it succeeded.
 */
esp_err_t en2m_attribute_write(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                               en2m_value_t value);

/** @brief Read the cached attribute value. */
esp_err_t en2m_attribute_get(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                             en2m_value_t *out_value);

/** @brief Convenience wrappers around ::en2m_attribute_set for common clusters. */
esp_err_t en2m_report_on_off(uint8_t endpoint_id, bool on);
esp_err_t en2m_report_level(uint8_t endpoint_id, uint8_t level);
esp_err_t en2m_report_color_temperature(uint8_t endpoint_id, uint16_t mireds);
esp_err_t en2m_report_temperature(uint8_t endpoint_id, int16_t centi_celsius);
esp_err_t en2m_report_humidity(uint8_t endpoint_id, uint16_t centi_percent);
esp_err_t en2m_report_pressure(uint8_t endpoint_id, int32_t deci_hpa);
esp_err_t en2m_report_illuminance(uint8_t endpoint_id, uint32_t lux);
esp_err_t en2m_report_boolean_state(uint8_t endpoint_id, bool state_value);
esp_err_t en2m_report_occupancy(uint8_t endpoint_id, bool occupied);
esp_err_t en2m_report_smoke(uint8_t endpoint_id, bool alarm);
esp_err_t en2m_report_co(uint8_t endpoint_id, bool alarm);
esp_err_t en2m_report_lock_state(uint8_t endpoint_id, en2m_lock_state_t state);
esp_err_t en2m_report_cover_position(uint8_t endpoint_id, uint8_t closed_percent);
esp_err_t en2m_report_fan(uint8_t endpoint_id, en2m_fan_mode_t mode, uint8_t percent);
esp_err_t en2m_report_power(uint8_t endpoint_id, int32_t milliwatts);
esp_err_t en2m_report_energy(uint8_t endpoint_id, int64_t milliwatt_hours);

/**
 * @brief Record one button press and report it.
 *
 * Increments ::EN2M_ATTR_PRESS_COUNT and stores @p action, which serializes to
 * `{"button": <count>, "button_action": "double_press"}`. The counter is what
 * makes a press detectable: reports are full snapshots and the state topic is
 * retained, so two identical payloads are indistinguishable from one payload
 * sent twice. Pressing the same button twice must therefore change *something*,
 * and the counter is that something. Callers never manage it.
 *
 * Presses arriving faster than `min_report_interval_ms` coalesce into one
 * report. The counter still advances once per press, so a receiver can see
 * from the gap that it missed some.
 */
esp_err_t en2m_report_button(uint8_t endpoint_id, en2m_press_action_t action);

/**
 * @brief ISR-safe ::en2m_report_button, for a GPIO button driver's callback.
 *
 * The increment happens on the en2m task, so the counter cannot be torn by two
 * presses racing on different cores.
 */
esp_err_t en2m_report_button_from_isr(uint8_t endpoint_id, en2m_press_action_t action,
                                      BaseType_t *higher_prio_task_woken);

#ifdef __cplusplus
}
#endif
