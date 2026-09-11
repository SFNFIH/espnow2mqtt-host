/**
 * @file en2m_model.h
 * @brief Interaction layer: lifecycle, endpoints, clusters, attributes, reporting.
 *
 * Layering, deliberately the same split ESP-Matter uses:
 *
 *   application + hardware drivers
 *        ↑ callbacks          ↓ en2m_attribute_set / en2m_attribute_write
 *   en2m model  (endpoints, clusters, attributes, commands, reporting)
 *        ↓
 *   en2m mesh   (ESP-NOW tree, one task, retries)
 *
 * The component owns the state, the task and the timing. The application owns
 * the hardware and reacts to callbacks. A device firmware therefore has no
 * loop of its own:
 *
 * @code
 * static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
 * {
 *     if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
 *         return relay_set(value->v.b);
 *     }
 *     return ESP_ERR_NOT_SUPPORTED;
 * }
 *
 * void app_main(void)
 * {
 *     en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
 *
 *     en2m_device_config_t cfg = {
 *         .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"},
 *         .attribute_write = on_write,
 *     };
 *     ESP_ERROR_CHECK(en2m_start(&cfg));
 * }
 * @endcode
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "en2m_attr.h"
#include "en2m_mesh.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_EN2M_MAX_ENDPOINTS
#define EN2M_MAX_ENDPOINTS CONFIG_EN2M_MAX_ENDPOINTS
#else
#define EN2M_MAX_ENDPOINTS 4
#endif

#ifdef CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT
#define EN2M_MAX_CLUSTERS_PER_ENDPOINT CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT
#else
#define EN2M_MAX_CLUSTERS_PER_ENDPOINT 8
#endif

#ifdef CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER
#define EN2M_MAX_ATTRIBUTES_PER_CLUSTER CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER
#else
#define EN2M_MAX_ATTRIBUTES_PER_CLUSTER 6
#endif

/** Opaque endpoint handle. */
typedef struct en2m_endpoint en2m_endpoint_t;

/** Opaque cluster handle. */
typedef struct en2m_cluster en2m_cluster_t;

/** When a state report is put on the air. */
typedef enum {
    EN2M_REPORT_DEFAULT = 0,    /**< On change (rate limited) plus a periodic keepalive */
    EN2M_REPORT_PERIODIC_ONLY,  /**< Only on the periodic interval */
    EN2M_REPORT_ON_CHANGE_ONLY, /**< Only when an attribute changes */
    EN2M_REPORT_MANUAL,         /**< Only when the application calls ::en2m_report_now */
} en2m_report_mode_t;

/** Pre-canned endpoint recipes, mirroring Matter device types. */
typedef enum {
    EN2M_DEVICE_TYPE_ON_OFF_LIGHT,
    EN2M_DEVICE_TYPE_DIMMABLE_LIGHT,
    EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT,
    EN2M_DEVICE_TYPE_ON_OFF_PLUG,
    EN2M_DEVICE_TYPE_SMART_PLUG,
    EN2M_DEVICE_TYPE_CONTACT_SENSOR,
    EN2M_DEVICE_TYPE_OCCUPANCY_SENSOR,
    EN2M_DEVICE_TYPE_LIGHT_SENSOR,
    EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR,
    EN2M_DEVICE_TYPE_HUMIDITY_SENSOR,
    EN2M_DEVICE_TYPE_PRESSURE_SENSOR,
    EN2M_DEVICE_TYPE_SMOKE_CO_ALARM,
    EN2M_DEVICE_TYPE_FAN,
    EN2M_DEVICE_TYPE_WINDOW_COVERING,
    EN2M_DEVICE_TYPE_DOOR_LOCK,
    EN2M_DEVICE_TYPE_THERMOSTAT,
} en2m_device_type_t;

/** Work item deferred onto the en2m task. */
typedef void (*en2m_work_fn_t)(void *arg);

/** Everything the component needs to run a device. */
typedef struct {
    en2m_config_t mesh; /**< Transport configuration (role, name, model, channel) */

    /** Actuators: apply a write to hardware. See ::en2m_attribute_write_cb_t. */
    en2m_attribute_write_cb_t attribute_write;
    /** Pull-style sensors: refresh a value before each report. */
    en2m_attribute_read_cb_t attribute_read;
    /** Observe committed changes. */
    en2m_attribute_changed_cb_t attribute_changed;
    /** Commands the application implements itself. */
    en2m_command_handler_t command;
    /** Identify effect. */
    en2m_identify_cb_t identify;
    /** Passed back to all of the above. */
    void *user_ctx;

    en2m_report_mode_t report_mode;
    uint32_t report_interval_ms;     /**< Periodic keepalive; 0 = role default */
    uint32_t min_report_interval_ms; /**< Floor between change reports; 0 = 1000 ms */
    uint32_t task_stack_size;        /**< 0 = 4096 */
    uint8_t task_priority;           /**< 0 = 5 */
} en2m_device_config_t;

/* ---- Data model ---- */

/**
 * @brief Create an endpoint (id 1..254). Call before ::en2m_start.
 */
en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id);

/** @brief Look up an existing endpoint. */
en2m_endpoint_t *en2m_endpoint_get(uint8_t endpoint_id);

/**
 * @brief Create an endpoint pre-populated for a device type.
 *
 * Adds the clusters and the default attributes that type needs, so a typical
 * firmware needs exactly one call. Add extra clusters afterwards as needed.
 */
en2m_endpoint_t *en2m_endpoint_create_device(uint8_t endpoint_id, en2m_device_type_t type);

/** @brief Add the clusters of a device type to an existing endpoint. */
esp_err_t en2m_endpoint_add_device_type(en2m_endpoint_t *endpoint, en2m_device_type_t type);

/**
 * @brief Add a cluster, pre-populated with the well-known attributes it owns.
 *
 * Returns the existing cluster when it was already added.
 */
en2m_cluster_t *en2m_cluster_create(en2m_endpoint_t *endpoint, uint16_t cluster_id);

/** @brief Look up a cluster on an endpoint. */
en2m_cluster_t *en2m_cluster_get(en2m_endpoint_t *endpoint, uint16_t cluster_id);

/**
 * @brief Add an attribute to a cluster.
 *
 * @param persist Keep the value across reboots in NVS. Use it for actuator
 *                state (relay, level, setpoint), not for sensor readings.
 */
esp_err_t en2m_attribute_create(en2m_cluster_t *cluster, uint16_t attribute_id,
                                en2m_value_t default_value, bool persist);

/**
 * @brief Install per-cluster callbacks, tried before the device-wide ones.
 *
 * Useful when one firmware drives several unrelated peripherals: each cluster
 * gets its own handler and its own context instead of one big switch.
 */
esp_err_t en2m_cluster_set_write_cb(en2m_cluster_t *cluster, en2m_attribute_write_cb_t cb, void *ctx);
esp_err_t en2m_cluster_set_read_cb(en2m_cluster_t *cluster, en2m_attribute_read_cb_t cb, void *ctx);
esp_err_t en2m_cluster_set_command_cb(en2m_cluster_t *cluster, en2m_command_handler_t cb, void *ctx);

/* ---- Lifecycle ---- */

/**
 * @brief Bring up the data model and the mesh transport.
 *
 * Starts one component-owned task that serves ESP-NOW receives, mesh
 * maintenance, retries and reporting. The application must not poll anything.
 */
esp_err_t en2m_start(const en2m_device_config_t *config);

/** @brief Stop the task and tear down the transport. The data model is kept. */
esp_err_t en2m_stop(void);

/** @brief True once ::en2m_start succeeded. */
bool en2m_is_started(void);

/* ---- Reporting ---- */

/** @brief Build and transmit a state report now. */
esp_err_t en2m_report_now(void);

/**
 * @brief Ask for a report in @p delay_ms, coalescing with pending requests.
 *
 * Respects `min_report_interval_ms`, so bursty sensors cannot flood the mesh.
 */
esp_err_t en2m_report_schedule(uint32_t delay_ms);

/* ---- Deferred work ---- */

/**
 * @brief Run @p fn on the en2m task.
 *
 * The escape hatch for work that must not happen in an ISR or in a callback:
 * a debounced button, an I2C transaction, a retry. It removes the last reason
 * for an application to own a task.
 */
esp_err_t en2m_schedule(en2m_work_fn_t fn, void *arg);

/** @brief ISR-safe ::en2m_schedule. */
esp_err_t en2m_schedule_from_isr(en2m_work_fn_t fn, void *arg, BaseType_t *higher_prio_task_woken);

/* ---- Deprecated compatibility shims ---- */

/** @deprecated Use ::en2m_start. */
esp_err_t en2m_model_start(const en2m_config_t *mesh_config);

/** @deprecated No-op. The component owns its task; nothing needs to be pumped. */
void en2m_model_loop(void);

/** @deprecated Use ::en2m_report_now. */
esp_err_t en2m_model_report(void);

/** @deprecated Use ::en2m_attribute_set, which reports the change by itself. */
esp_err_t en2m_model_notify(uint8_t endpoint_id, uint16_t cluster_id, bool immediate);

#ifdef __cplusplus
}
#endif
