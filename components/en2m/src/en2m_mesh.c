/**
 * @file en2m_mesh.c
 * @brief ESP-NOW mesh tree implementation.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "en2m_mesh.h"
#include "en2m_priv.h"

static const char *TAG = "en2m";

/** File-static mesh context (ESP-IDF: s_ prefix). */
static en2m_ctx_t s_ctx;

static int64_t en2m_now_us(void)
{
    return esp_timer_get_time();
}

static int64_t en2m_now_ms(void)
{
    return en2m_now_us() / 1000;
}

static void en2m_emit_log(const char *msg)
{
    if (s_ctx.config.on_log != NULL) {
        s_ctx.config.on_log(msg, s_ctx.config.user_ctx);
    } else {
        ESP_LOGI(TAG, "%s", msg);
    }
}

static bool en2m_add_peer(const uint8_t mac[6])
{
    esp_now_peer_info_t peer = {0};

    if (esp_now_is_peer_exist(mac)) {
        return true;
    }

    en2m_mac_copy(peer.peer_addr, mac);
    peer.channel = s_ctx.config.channel ? s_ctx.config.channel : EN2M_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

static void en2m_fill_identity(en2m_pkt_t *pkt)
{
    const char *model;

    memset(pkt, 0, sizeof(*pkt));
    pkt->magic = EN2M_MAGIC;
    pkt->version = EN2M_VERSION;
    pkt->role = (uint8_t)s_ctx.config.role;
    pkt->hop_limit = EN2M_HOP_LIMIT;
    pkt->seq = s_ctx.seq++;

    model = (s_ctx.config.model != NULL) ? s_ctx.config.model : EN2M_DEVICE_MODEL;
    strncpy(pkt->model, model, sizeof(pkt->model) - 1);
    strncpy(pkt->name, s_ctx.name, sizeof(pkt->name) - 1);
}

static esp_err_t en2m_send_raw(const uint8_t to[6], en2m_pkt_t *pkt)
{
    if (!en2m_add_peer(to)) {
        return ESP_FAIL;
    }
    return esp_now_send(to, (uint8_t *)pkt, sizeof(*pkt));
}

static void en2m_remember_neighbor(const uint8_t mac[6], uint8_t role, uint8_t cost, int8_t rssi)
{
    int free_idx = -1;

    for (int i = 0; i < EN2M_MAX_NEIGHBORS; i++) {
        if (s_ctx.neighbors[i].used && en2m_mac_equal(s_ctx.neighbors[i].mac, mac)) {
            s_ctx.neighbors[i].role = role;
            s_ctx.neighbors[i].cost = cost;
            s_ctx.neighbors[i].rssi = rssi;
            s_ctx.neighbors[i].last_us = en2m_now_us();
            return;
        }
        if (!s_ctx.neighbors[i].used && free_idx < 0) {
            free_idx = i;
        }
    }

    if (free_idx < 0) {
        return;
    }

    s_ctx.neighbors[free_idx].used = true;
    en2m_mac_copy(s_ctx.neighbors[free_idx].mac, mac);
    s_ctx.neighbors[free_idx].role = role;
    s_ctx.neighbors[free_idx].cost = cost;
    s_ctx.neighbors[free_idx].rssi = rssi;
    s_ctx.neighbors[free_idx].last_us = en2m_now_us();
}

static void en2m_learn_route(const uint8_t origin[6], const uint8_t from[6], uint8_t hop)
{
    int free_idx = -1;

    if (en2m_mac_equal(origin, s_ctx.self_mac)) {
        return;
    }

    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (s_ctx.routes[i].used && en2m_mac_equal(s_ctx.routes[i].dest, origin)) {
            if (hop <= s_ctx.routes[i].hop) {
                en2m_mac_copy(s_ctx.routes[i].next_hop, from);
                s_ctx.routes[i].hop = hop;
            }
            s_ctx.routes[i].last_us = en2m_now_us();
            return;
        }
        if (!s_ctx.routes[i].used && free_idx < 0) {
            free_idx = i;
        }
    }

    if (free_idx < 0) {
        return;
    }

    s_ctx.routes[free_idx].used = true;
    en2m_mac_copy(s_ctx.routes[free_idx].dest, origin);
    en2m_mac_copy(s_ctx.routes[free_idx].next_hop, from);
    s_ctx.routes[free_idx].hop = hop;
    s_ctx.routes[free_idx].last_us = en2m_now_us();
}

bool en2m_lookup_route(const uint8_t dest[6], uint8_t next_hop[6])
{
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (s_ctx.routes[i].used && en2m_mac_equal(s_ctx.routes[i].dest, dest)) {
            en2m_mac_copy(next_hop, s_ctx.routes[i].next_hop);
            return true;
        }
    }
    return false;
}

void en2m_forget_route(const uint8_t dest[6])
{
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (s_ctx.routes[i].used && en2m_mac_equal(s_ctx.routes[i].dest, dest)) {
            s_ctx.routes[i].used = false;
        }
    }
}

void en2m_clear_routes(void)
{
    memset(s_ctx.routes, 0, sizeof(s_ctx.routes));
}

static void en2m_expire_routes(void)
{
    int64_t now_ms = en2m_now_ms();

    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (s_ctx.routes[i].used && (now_ms - s_ctx.routes[i].last_us / 1000) > EN2M_ROUTE_STALE_MS) {
            s_ctx.routes[i].used = false;
        }
    }

    for (int i = 0; i < EN2M_MAX_NEIGHBORS; i++) {
        if (s_ctx.neighbors[i].used &&
            (now_ms - s_ctx.neighbors[i].last_us / 1000) > (EN2M_PARENT_STALE_MS * 2)) {
            s_ctx.neighbors[i].used = false;
        }
    }
}

static void en2m_expire_parent(void)
{
    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        return;
    }
    if (!s_ctx.has_parent) {
        return;
    }
    if ((en2m_now_ms() - s_ctx.parent_last_us / 1000) > EN2M_PARENT_STALE_MS) {
        s_ctx.has_parent = false;
        s_ctx.path_cost = 255;
        en2m_emit_log("parent stale");
    }
}

static void en2m_consider_parent(const uint8_t mac[6], uint8_t role, uint8_t their_cost, int8_t rssi)
{
    uint8_t new_cost;
    bool better = false;
    char log_buf[64];
    char mac_str[18];

    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        return;
    }
    if (role != EN2M_ROLE_COORDINATOR && role != EN2M_ROLE_ROUTER) {
        return;
    }
    if (their_cost >= 254) {
        return;
    }

    new_cost = (uint8_t)(their_cost + 1);
    if (new_cost > EN2M_HOP_LIMIT) {
        return;
    }

    if (!s_ctx.has_parent) {
        better = true;
    } else if (new_cost < s_ctx.path_cost) {
        better = true;
    } else if (new_cost == s_ctx.path_cost && en2m_mac_equal(s_ctx.parent_mac, mac)) {
        s_ctx.parent_last_us = en2m_now_us();
        return;
    } else if (new_cost == s_ctx.path_cost) {
        int8_t cur_rssi = -100;
        for (int i = 0; i < EN2M_MAX_NEIGHBORS; i++) {
            if (s_ctx.neighbors[i].used && en2m_mac_equal(s_ctx.neighbors[i].mac, s_ctx.parent_mac)) {
                cur_rssi = s_ctx.neighbors[i].rssi;
                break;
            }
        }
        if (rssi > cur_rssi + 8) {
            better = true;
        }
    }

    if (!better) {
        return;
    }

    en2m_mac_copy(s_ctx.parent_mac, mac);
    s_ctx.has_parent = true;
    s_ctx.path_cost = new_cost;
    s_ctx.parent_last_us = en2m_now_us();
    en2m_add_peer(s_ctx.parent_mac);

    en2m_mac_to_str(s_ctx.parent_mac, mac_str);
    snprintf(log_buf, sizeof(log_buf), "parent=%s cost=%u rssi=%d", mac_str, s_ctx.path_cost, (int)rssi);
    en2m_emit_log(log_buf);
}

static void en2m_send_beacon(void)
{
    en2m_pkt_t pkt;
    uint8_t cost = 0;
    uint8_t bcast[6];

    if (s_ctx.config.role == EN2M_ROLE_LEAF) {
        return;
    }

    if (s_ctx.config.role == EN2M_ROLE_ROUTER) {
        if (!s_ctx.has_parent) {
            return;
        }
        cost = s_ctx.path_cost;
    }

    en2m_fill_identity(&pkt);
    pkt.msg_type = EN2M_MSG_BEACON;
    pkt.cost = cost;
    pkt.hop = 0;
    if (s_ctx.pairing) {
        pkt.flags |= EN2M_FLAG_PAIRING;
    }
    en2m_mac_copy(pkt.origin, s_ctx.self_mac);
    en2m_mac_broadcast(pkt.dest);
    en2m_mac_copy(pkt.via, s_ctx.self_mac);
    en2m_mac_broadcast(bcast);
    en2m_send_raw(bcast, &pkt);
}

static void en2m_forward_toward_coord(en2m_pkt_t *pkt)
{
    if (s_ctx.config.role != EN2M_ROLE_ROUTER || !s_ctx.has_parent) {
        return;
    }
    if (pkt->hop >= pkt->hop_limit) {
        return;
    }
    pkt->hop++;
    en2m_mac_copy(pkt->via, s_ctx.self_mac);
    en2m_send_raw(s_ctx.parent_mac, pkt);
}

static void en2m_forward_toward_dest(en2m_pkt_t *pkt)
{
    uint8_t next[6];

    if (s_ctx.config.role == EN2M_ROLE_LEAF) {
        return;
    }
    if (pkt->hop >= pkt->hop_limit) {
        return;
    }
    if (!en2m_lookup_route(pkt->dest, next)) {
        en2m_mac_copy(next, pkt->dest);
    }
    pkt->hop++;
    en2m_mac_copy(pkt->via, s_ctx.self_mac);
    en2m_send_raw(next, pkt);
}

static void en2m_handle_rx(const uint8_t from[6], const en2m_pkt_t *in, int8_t rssi)
{
    if (in->magic != EN2M_MAGIC || in->version != EN2M_VERSION) {
        return;
    }
    if (en2m_mac_equal(from, s_ctx.self_mac)) {
        return;
    }

    en2m_remember_neighbor(from, in->role, in->cost, rssi);

    if (in->msg_type == EN2M_MSG_BEACON) {
        en2m_consider_parent(from, in->role, in->cost, rssi);
        return;
    }

    if (in->msg_type == EN2M_MSG_CMD && en2m_mac_equal(in->dest, s_ctx.self_mac)) {
        if (s_ctx.config.on_command != NULL) {
            s_ctx.config.on_command(in, s_ctx.config.user_ctx);
        }
        return;
    }

    if (s_ctx.config.role == EN2M_ROLE_ROUTER && in->msg_type == EN2M_MSG_CMD &&
        !en2m_mac_equal(in->dest, s_ctx.self_mac)) {
        en2m_pkt_t pkt = *in;
        en2m_learn_route(in->origin, from, in->hop);
        en2m_forward_toward_dest(&pkt);
        return;
    }

    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        bool uplink = (in->msg_type == EN2M_MSG_HELLO || in->msg_type == EN2M_MSG_STATE ||
                       in->msg_type == EN2M_MSG_ACK || in->msg_type == EN2M_MSG_HEARTBEAT);
        uint8_t existing_next[6];
        bool known;

        if (!uplink) {
            return;
        }

        known = en2m_lookup_route(in->origin, existing_next);
        if (!known && !s_ctx.pairing) {
            en2m_emit_log("drop uplink (not pairing / unknown)");
            return;
        }

        en2m_learn_route(in->origin, from, in->hop);
        if (s_ctx.config.on_uplink != NULL) {
            s_ctx.config.on_uplink(in, rssi, from, s_ctx.config.user_ctx);
        }
        return;
    }

    if (s_ctx.config.role == EN2M_ROLE_ROUTER &&
        (in->msg_type == EN2M_MSG_HELLO || in->msg_type == EN2M_MSG_STATE ||
         in->msg_type == EN2M_MSG_ACK || in->msg_type == EN2M_MSG_HEARTBEAT)) {
        en2m_pkt_t pkt = *in;
        en2m_learn_route(in->origin, from, in->hop);
        en2m_forward_toward_coord(&pkt);
    }
}

static void en2m_espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    int8_t rssi;

    if (!s_ctx.inited || info == NULL || data == NULL || len < (int)offsetof(en2m_pkt_t, data)) {
        return;
    }

    rssi = (info->rx_ctrl != NULL) ? info->rx_ctrl->rssi : 0;
    if (s_ctx.lock != NULL) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }
    en2m_handle_rx(info->src_addr, (const en2m_pkt_t *)data, rssi);
    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
}

static esp_err_t en2m_wifi_init(uint8_t channel)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), TAG, "set_channel failed");
    return ESP_OK;
}

esp_err_t en2m_mesh_init(const en2m_config_t *config)
{
    esp_err_t err;
    uint8_t bcast[6];

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->role == EN2M_ROLE_COORDINATOR || config->role == EN2M_ROLE_ROUTER ||
                            config->role == EN2M_ROLE_LEAF,
                        ESP_ERR_INVALID_ARG, TAG, "invalid role");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = *config;
    /* Map legacy field name if apps still set .user */
    if (s_ctx.config.user_ctx == NULL) {
        /* keep as-is */
    }
    if (s_ctx.config.channel == 0) {
        s_ctx.config.channel = EN2M_WIFI_CHANNEL;
    }
    strncpy(s_ctx.name, (config->name != NULL) ? config->name : EN2M_DEVICE_NAME, sizeof(s_ctx.name) - 1);
    s_ctx.path_cost = 255;
    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(en2m_wifi_init(s_ctx.config.channel), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, s_ctx.self_mac), TAG, "get_mac failed");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(en2m_espnow_recv_cb), TAG, "register_recv_cb failed");

    en2m_mac_broadcast(bcast);
    en2m_add_peer(bcast);

    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        s_ctx.path_cost = 0;
        s_ctx.has_parent = true;
    }

    s_ctx.inited = true;
    en2m_emit_log("mesh init");
    return ESP_OK;
}

void en2m_mesh_deinit(void)
{
    if (!s_ctx.inited) {
        return;
    }
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ctx.lock != NULL) {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }
    s_ctx.inited = false;
}

void en2m_mesh_loop(void)
{
    if (!s_ctx.inited) {
        return;
    }

    if (s_ctx.lock != NULL) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }

    en2m_expire_parent();
    en2m_expire_routes();

    if (s_ctx.config.role != EN2M_ROLE_LEAF) {
        if ((en2m_now_ms() - s_ctx.last_beacon_us / 1000) >= EN2M_BEACON_MS_DEFAULT) {
            s_ctx.last_beacon_us = en2m_now_us();
            en2m_send_beacon();
        }
    }

    if (s_ctx.config.role != EN2M_ROLE_COORDINATOR) {
        if ((en2m_now_ms() - s_ctx.last_hello_us / 1000) > 30000) {
            s_ctx.last_hello_us = en2m_now_us();
            en2m_send_uplink(EN2M_MSG_HEARTBEAT, 0, NULL, 0);
        }
    }

    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
}

esp_err_t en2m_send_uplink(uint8_t msg_type, uint16_t cmd_id, const uint8_t *data, uint8_t len)
{
    en2m_pkt_t pkt;
    uint8_t bcast[6];

    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_ctx.has_parent && msg_type != EN2M_MSG_HELLO && msg_type != EN2M_MSG_HEARTBEAT) {
        return ESP_ERR_INVALID_STATE;
    }

    en2m_fill_identity(&pkt);
    pkt.msg_type = msg_type;
    pkt.cmd_id = cmd_id;
    pkt.hop = 0;
    pkt.cost = s_ctx.path_cost;
    en2m_mac_copy(pkt.origin, s_ctx.self_mac);
    en2m_mac_broadcast(pkt.dest);
    en2m_mac_copy(pkt.via, s_ctx.self_mac);

    if (data != NULL && len > 0) {
        if (len > EN2M_DATA_MAX) {
            len = EN2M_DATA_MAX;
        }
        pkt.data_len = len;
        memcpy(pkt.data, data, len);
    }

    if (s_ctx.has_parent) {
        return en2m_send_raw(s_ctx.parent_mac, &pkt);
    }

    en2m_mac_broadcast(bcast);
    return en2m_send_raw(bcast, &pkt);
}

esp_err_t en2m_send_downlink(const uint8_t dest_mac[6], uint16_t cmd_id, const uint8_t *data, uint8_t len)
{
    en2m_pkt_t pkt;
    uint8_t next[6];

    ESP_RETURN_ON_FALSE(dest_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "dest_mac is NULL");
    if (s_ctx.config.role != EN2M_ROLE_COORDINATOR) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    en2m_fill_identity(&pkt);
    pkt.msg_type = EN2M_MSG_CMD;
    pkt.cmd_id = cmd_id;
    pkt.hop = 0;
    pkt.cost = 0;
    en2m_mac_copy(pkt.origin, s_ctx.self_mac);
    en2m_mac_copy(pkt.dest, dest_mac);
    en2m_mac_copy(pkt.via, s_ctx.self_mac);

    if (data != NULL && len > 0) {
        if (len > EN2M_DATA_MAX) {
            len = EN2M_DATA_MAX;
        }
        pkt.data_len = len;
        memcpy(pkt.data, data, len);
    }

    if (en2m_lookup_route(dest_mac, next)) {
        return en2m_send_raw(next, &pkt);
    }
    return en2m_send_raw(dest_mac, &pkt);
}

void en2m_set_pairing(bool enabled)
{
    s_ctx.pairing = enabled;
}

bool en2m_get_pairing(void)
{
    return s_ctx.pairing;
}

bool en2m_has_parent(void)
{
    return s_ctx.has_parent;
}

uint8_t en2m_get_path_cost(void)
{
    return s_ctx.path_cost;
}

en2m_role_t en2m_get_role(void)
{
    return s_ctx.config.role;
}

void en2m_get_parent_mac(uint8_t out[6])
{
    if (s_ctx.has_parent) {
        en2m_mac_copy(out, s_ctx.parent_mac);
    } else {
        en2m_mac_broadcast(out);
    }
}

void en2m_get_self_mac(uint8_t out[6])
{
    en2m_mac_copy(out, s_ctx.self_mac);
}

void en2m_set_name(const char *name)
{
    if (name == NULL) {
        return;
    }
    strncpy(s_ctx.name, name, sizeof(s_ctx.name) - 1);
    s_ctx.name[sizeof(s_ctx.name) - 1] = '\0';
}
