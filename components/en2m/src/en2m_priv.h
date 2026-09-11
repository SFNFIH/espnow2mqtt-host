/**
 * @file en2m_priv.h
 * @brief Internals shared between the en2m translation units. Not a public API.
 */

#pragma once

#include <stdint.h>

#include "en2m_attr.h"
#include "en2m_event.h"
#include "en2m_mesh.h"
#include "en2m_model.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Period of the maintenance tick run by the en2m task. */
#define EN2M_TICK_MS 100

/* ---- Data model storage ---- */

typedef struct {
    bool used;
    uint16_t id;
    bool persist;
    bool persist_dirty;
    en2m_value_t value;
} en2m_attr_slot_t;

struct en2m_cluster {
    bool used;
    uint16_t id;
    uint8_t endpoint_id;
    en2m_attribute_write_cb_t write_cb;
    void *write_ctx;
    en2m_attribute_read_cb_t read_cb;
    void *read_ctx;
    en2m_command_handler_t command_cb;
    void *command_ctx;
    en2m_attr_slot_t attrs[EN2M_MAX_ATTRIBUTES_PER_CLUSTER];
};

struct en2m_endpoint {
    bool used;
    uint8_t id;
    struct en2m_cluster clusters[EN2M_MAX_CLUSTERS_PER_ENDPOINT];
};

/* ---- Mesh state ---- */

typedef struct {
    bool used;
    uint8_t mac[6];
    uint8_t role;
    uint8_t cost;
    int8_t rssi;
    int64_t last_us;
} en2m_neighbor_t;

typedef struct {
    bool used;
    uint8_t dest[6];
    uint8_t next_hop[6];
    uint8_t hop;
    int64_t last_us;
} en2m_route_t;

/** A downlink awaiting its ACK. */
typedef struct {
    bool used;
    uint8_t dest[6];
    uint16_t cmd_id;
    uint8_t attempts;
    int64_t next_us;
    en2m_pkt_t pkt;
} en2m_pending_t;

/** Work the en2m task has to do, funnelled through one queue to keep order. */
typedef enum {
    EN2M_ITEM_RX,   /**< A frame received by the ESP-NOW callback */
    EN2M_ITEM_WORK, /**< An application function deferred out of an ISR */
    EN2M_ITEM_ATTR, /**< An attribute commit deferred out of an ISR */
} en2m_item_kind_t;

typedef struct {
    en2m_item_kind_t kind;
    union {
        struct {
            uint8_t src[6];
            int8_t rssi;
            en2m_pkt_t pkt;
        } rx;
        struct {
            en2m_work_fn_t fn;
            void *arg;
        } work;
        struct {
            en2m_attr_path_t path;
            en2m_value_t value;
        } attr;
    } u;
} en2m_item_t;

typedef struct {
    bool inited;
    bool running;
    en2m_config_t config;
    char name[16];
    uint8_t self_mac[6];
    uint32_t seq;
    uint16_t next_cmd_id;
    bool pairing;
    bool has_parent;
    uint8_t parent_mac[6];
    uint8_t path_cost;
    int64_t parent_last_us;
    en2m_neighbor_t neighbors[EN2M_MAX_NEIGHBORS];
    en2m_route_t routes[EN2M_MAX_ROUTES];
    en2m_pending_t pending[EN2M_MAX_PENDING];
    int64_t last_beacon_us;
    int64_t last_hello_us;
    int64_t last_tick_ms;
    uint32_t rx_dropped;
    SemaphoreHandle_t lock;
    QueueHandle_t queue;
    TaskHandle_t task;
} en2m_ctx_t;

/* ---- events ---- */

/** Publish an en2m event; @p data may be NULL when @p size is 0. */
void en2m_event_post(en2m_event_id_t id, const void *data, size_t size);

/* ---- mesh internals used by the model ---- */

/** Post an item to the en2m task queue. */
esp_err_t en2m_task_post(const en2m_item_t *item, bool from_isr, BaseType_t *higher_prio_task_woken);

/** True when the en2m task is up and able to accept queue items. */
bool en2m_task_running(void);

/** True when the caller already runs on the en2m task. */
bool en2m_task_is_current(void);

/* ---- data model internals used by the model layer ---- */

void en2m_dm_lock(void);
void en2m_dm_unlock(void);

/** Raw slot access; index is 0..EN2M_MAX_ENDPOINTS-1. Caller holds the lock. */
struct en2m_endpoint *en2m_dm_endpoint_slot(int index);
struct en2m_cluster *en2m_dm_find_cluster(uint8_t endpoint_id, uint16_t cluster_id);
en2m_attr_slot_t *en2m_dm_find_attr(struct en2m_cluster *cluster, uint16_t attribute_id);

/** Load persisted attribute values from NVS. Called once during start. */
void en2m_dm_restore(void);

/** Write back attributes whose persisted copy is stale. */
void en2m_dm_flush_persist(void);

/** Refresh every attribute that has a read callback, straight before a report. */
void en2m_dm_refresh(void);

/* ---- model internals used by the mesh and the data model ---- */

/** Effective device configuration, or NULL before ::en2m_start. */
const en2m_device_config_t *en2m_model_config(void);

/** Called by the data model after a value was committed. */
void en2m_model_on_change(const en2m_attr_path_t *path, const en2m_value_t *value);

/** Called by the en2m task for a CMD frame addressed to this node. */
void en2m_model_on_command_frame(const en2m_pkt_t *pkt);

/** Called by the en2m task once per ::EN2M_TICK_MS. */
void en2m_model_tick(int64_t now_ms);

/** Called by the mesh when the uplink path appears or disappears. */
void en2m_model_on_link_change(bool has_parent);

#ifdef __cplusplus
}
#endif
