/**
 * @file en2m_mesh.h
 * @brief ESP-NOW mesh transport (tree rooted at the USB coordinator).
 *
 * Roles (see ::en2m_role_t):
 * - COORDINATOR: USB stick, cost 0, accepts uplink, sends downlink
 * - ROUTER: mains-powered, rebroadcasts beacons, forwards both ways
 * - LEAF: no forwarding, attaches to the best parent only
 *
 * The transport owns a task. Receives are queued out of the ESP-NOW callback,
 * maintenance runs on a fixed tick, and downlinks are retried until they are
 * acknowledged. Applications never pump this layer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "en2m_proto.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults from Kconfig when available; otherwise safe built-ins. */
#ifdef CONFIG_EN2M_WIFI_CHANNEL
#define EN2M_WIFI_CHANNEL CONFIG_EN2M_WIFI_CHANNEL
#else
#define EN2M_WIFI_CHANNEL 1
#endif

#ifdef CONFIG_EN2M_HOP_LIMIT
#define EN2M_HOP_LIMIT CONFIG_EN2M_HOP_LIMIT
#else
#define EN2M_HOP_LIMIT 8
#endif

#ifdef CONFIG_EN2M_MAX_ROUTES
#define EN2M_MAX_ROUTES CONFIG_EN2M_MAX_ROUTES
#else
#define EN2M_MAX_ROUTES 32
#endif

#ifdef CONFIG_EN2M_MAX_NEIGHBORS
#define EN2M_MAX_NEIGHBORS CONFIG_EN2M_MAX_NEIGHBORS
#else
#define EN2M_MAX_NEIGHBORS 16
#endif

#ifdef CONFIG_EN2M_BEACON_INTERVAL_MS
#define EN2M_BEACON_MS_DEFAULT CONFIG_EN2M_BEACON_INTERVAL_MS
#else
#define EN2M_BEACON_MS_DEFAULT 5000
#endif

#ifdef CONFIG_EN2M_PARENT_STALE_MS
#define EN2M_PARENT_STALE_MS CONFIG_EN2M_PARENT_STALE_MS
#else
#define EN2M_PARENT_STALE_MS 20000
#endif

#ifdef CONFIG_EN2M_ROUTE_STALE_MS
#define EN2M_ROUTE_STALE_MS CONFIG_EN2M_ROUTE_STALE_MS
#else
#define EN2M_ROUTE_STALE_MS 120000
#endif

#ifdef CONFIG_EN2M_OFFLINE_MS
#define EN2M_OFFLINE_MS CONFIG_EN2M_OFFLINE_MS
#else
#define EN2M_OFFLINE_MS 90000
#endif

#ifdef CONFIG_EN2M_HEARTBEAT_MS
#define EN2M_HEARTBEAT_MS CONFIG_EN2M_HEARTBEAT_MS
#else
#define EN2M_HEARTBEAT_MS 30000
#endif

#ifdef CONFIG_EN2M_QUEUE_LEN
#define EN2M_QUEUE_LEN CONFIG_EN2M_QUEUE_LEN
#else
#define EN2M_QUEUE_LEN 8
#endif

#ifdef CONFIG_EN2M_MAX_PENDING
#define EN2M_MAX_PENDING CONFIG_EN2M_MAX_PENDING
#else
#define EN2M_MAX_PENDING 4
#endif

#ifdef CONFIG_EN2M_CMD_RETRIES
#define EN2M_CMD_RETRIES CONFIG_EN2M_CMD_RETRIES
#else
#define EN2M_CMD_RETRIES 3
#endif

#ifdef CONFIG_EN2M_CMD_RETRY_MS
#define EN2M_CMD_RETRY_MS CONFIG_EN2M_CMD_RETRY_MS
#else
#define EN2M_CMD_RETRY_MS 400
#endif

#ifndef EN2M_DEVICE_MODEL
#define EN2M_DEVICE_MODEL "c3-node"
#endif

#ifndef EN2M_DEVICE_NAME
#define EN2M_DEVICE_NAME "node1"
#endif

#ifndef EN2M_FW_VERSION
#define EN2M_FW_VERSION "0.4.0-idf"
#endif

typedef void (*en2m_uplink_cb_t)(const en2m_pkt_t *pkt, int8_t rssi, const uint8_t from_mac[6], void *user_ctx);
typedef void (*en2m_frame_cb_t)(const en2m_pkt_t *pkt, void *user_ctx);
typedef void (*en2m_log_cb_t)(const char *msg, void *user_ctx);

/** @deprecated Use ::en2m_frame_cb_t. */
typedef en2m_frame_cb_t en2m_command_cb_t;

/**
 * @brief Transport configuration.
 */
typedef struct {
    en2m_role_t role;             /**< Node role */
    const char *model;            /**< Short model id (copied into packets) */
    const char *name;             /**< Friendly name / MQTT slug */
    const char *fw;               /**< Firmware version string */
    uint8_t channel;              /**< 0 = use EN2M_WIFI_CHANNEL */
    en2m_uplink_cb_t on_uplink;   /**< Coordinator: frames for the host bridge */
    en2m_frame_cb_t on_command;   /**< Raw CMD frames addressed to this node; the
                                       interaction layer installs its own decoder
                                       when this is NULL */
    en2m_log_cb_t on_log;         /**< Optional log sink; NULL uses ESP_LOG */
    void *user_ctx;               /**< Passed to the callbacks */
    uint8_t max_retries;          /**< Downlink retries before giving up; 0 = default */
    uint32_t retry_interval_ms;   /**< Delay between retries; 0 = default */
} en2m_config_t;

/** @deprecated Prefer ::en2m_config_t */
typedef en2m_config_t en2m_app_config_t;

/**
 * @brief Initialize Wi-Fi (STA, no AP join), ESP-NOW, the mesh state and the task.
 *
 * ::en2m_start calls this for a device firmware. A pure router or the
 * coordinator, which have no clusters, may call it directly.
 */
esp_err_t en2m_mesh_init(const en2m_config_t *config);

/**
 * @brief Tear down the task, ESP-NOW and Wi-Fi started by ::en2m_mesh_init.
 */
void en2m_mesh_deinit(void);

/**
 * @brief Send an uplink frame toward the coordinator (leaf/router).
 */
esp_err_t en2m_send_uplink(uint8_t msg_type, uint16_t cmd_id, const uint8_t *data, uint8_t len);

/**
 * @brief Send a downlink CMD to @p dest_mac (coordinator only).
 *
 * When @p cmd_id is non-zero the frame is retried until the node acknowledges
 * it; ::EN2M_EVENT_ACK_RECEIVED or ::EN2M_EVENT_ACK_TIMEOUT reports the outcome.
 */
esp_err_t en2m_send_downlink(const uint8_t dest_mac[6], uint16_t cmd_id, const uint8_t *data, uint8_t len);

/** @brief Allocate a transaction id for ::en2m_send_downlink (never 0). */
uint16_t en2m_next_cmd_id(void);

void en2m_set_pairing(bool enabled);
bool en2m_get_pairing(void);

bool en2m_has_parent(void);
uint8_t en2m_get_path_cost(void);
en2m_role_t en2m_get_role(void);
void en2m_get_parent_mac(uint8_t out[6]);
void en2m_get_self_mac(uint8_t out[6]);

bool en2m_lookup_route(const uint8_t dest[6], uint8_t next_hop[6]);
void en2m_forget_route(const uint8_t dest[6]);
void en2m_clear_routes(void);
void en2m_set_name(const char *name);

/** @deprecated No-op. The transport runs its own task. */
void en2m_mesh_loop(void);

/* Compatibility aliases (older example code). */
static inline bool en2m_pairing(void)
{
    return en2m_get_pairing();
}

static inline uint8_t en2m_path_cost(void)
{
    return en2m_get_path_cost();
}

static inline uint8_t en2m_role(void)
{
    return (uint8_t)en2m_get_role();
}

static inline void en2m_parent_mac(uint8_t out[6])
{
    en2m_get_parent_mac(out);
}

static inline void en2m_self_mac(uint8_t out[6])
{
    en2m_get_self_mac(out);
}

#ifdef __cplusplus
}
#endif
