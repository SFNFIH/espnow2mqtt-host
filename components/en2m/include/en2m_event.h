/**
 * @file en2m_event.h
 * @brief en2m events, published on the ESP-IDF default event loop.
 *
 * Every asynchronous thing the stack does is observable without polling:
 *
 * @code
 * static void on_en2m(void *arg, esp_event_base_t base, int32_t id, void *data)
 * {
 *     if (id == EN2M_EVENT_PARENT_FOUND) {
 *         const en2m_event_parent_t *p = data;
 *         ESP_LOGI(TAG, "joined via cost %u rssi %d", p->cost, p->rssi);
 *     }
 * }
 *
 * en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m, NULL);
 * @endcode
 *
 * Handlers run on the default event loop task, never in an ISR and never in
 * the ESP-NOW receive callback, so they may call any en2m API.
 */

#pragma once

#include <stdint.h>

#include "en2m_attr.h"
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Event base for all en2m events. */
ESP_EVENT_DECLARE_BASE(EN2M_EVENT);

/** Subscribe to every en2m event. */
#define EN2M_EVENT_ANY ESP_EVENT_ANY_ID

/** en2m event identifiers. */
typedef enum {
    EN2M_EVENT_STARTED,            /**< no data — stack is up */
    EN2M_EVENT_STOPPED,            /**< no data */
    EN2M_EVENT_PARENT_FOUND,       /**< ::en2m_event_parent_t */
    EN2M_EVENT_PARENT_LOST,        /**< ::en2m_event_parent_t (last known parent) */
    EN2M_EVENT_PAIRING_CHANGED,    /**< ::en2m_event_pairing_t */
    EN2M_EVENT_ATTRIBUTE_UPDATED,  /**< ::en2m_event_attribute_t */
    EN2M_EVENT_COMMAND_RECEIVED,   /**< ::en2m_event_command_t */
    EN2M_EVENT_REPORT_SENT,        /**< ::en2m_event_report_t */
    EN2M_EVENT_IDENTIFY,           /**< ::en2m_event_identify_t */
    EN2M_EVENT_ACK_RECEIVED,       /**< ::en2m_event_ack_t — coordinator only */
    EN2M_EVENT_ACK_TIMEOUT,        /**< ::en2m_event_ack_t — coordinator only */
    EN2M_EVENT_RX_DROPPED,         /**< ::en2m_event_dropped_t */
} en2m_event_id_t;

typedef struct {
    uint8_t mac[6];
    uint8_t cost;
    int8_t rssi;
} en2m_event_parent_t;

typedef struct {
    bool enabled;
} en2m_event_pairing_t;

typedef struct {
    en2m_attr_path_t path;
    en2m_value_t value;
} en2m_event_attribute_t;

typedef struct {
    uint8_t endpoint_id;
    uint16_t cluster_id;
    en2m_command_id_t id;
    uint16_t transaction_id;
} en2m_event_command_t;

typedef struct {
    uint16_t length;   /**< Payload bytes actually sent */
    bool truncated;    /**< Report did not fit and was shortened */
    esp_err_t err;     /**< Transmit result */
} en2m_event_report_t;

typedef struct {
    uint8_t endpoint_id;
    uint16_t seconds;
} en2m_event_identify_t;

typedef struct {
    uint8_t mac[6];
    uint16_t transaction_id;
    uint8_t attempts;
} en2m_event_ack_t;

typedef struct {
    uint32_t total; /**< Cumulative frames dropped because the queue was full */
} en2m_event_dropped_t;

/**
 * @brief Register a handler for one event id, or ::EN2M_EVENT_ANY for all.
 *
 * Creates the default event loop if the application has not created it yet.
 */
esp_err_t en2m_event_handler_register(int32_t event_id, esp_event_handler_t handler, void *handler_arg);

/** @brief Remove a handler registered with ::en2m_event_handler_register. */
esp_err_t en2m_event_handler_unregister(int32_t event_id, esp_event_handler_t handler);

#ifdef __cplusplus
}
#endif
