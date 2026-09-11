/**
 * @file en2m_priv.h
 * @brief Private helpers for the en2m component (not for application use).
 */

#pragma once

#include <stdint.h>

#include "en2m_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct {
    bool inited;
    en2m_config_t config;
    char name[16];
    uint8_t self_mac[6];
    uint32_t seq;
    bool pairing;
    bool has_parent;
    uint8_t parent_mac[6];
    uint8_t path_cost;
    int64_t parent_last_us;
    en2m_neighbor_t neighbors[EN2M_MAX_NEIGHBORS];
    en2m_route_t routes[EN2M_MAX_ROUTES];
    int64_t last_beacon_us;
    int64_t last_hello_us;
    SemaphoreHandle_t lock;
} en2m_ctx_t;

#ifdef __cplusplus
}
#endif
