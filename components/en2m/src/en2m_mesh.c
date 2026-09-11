/**
 * @file en2m_mesh.c
 * @brief ESP-NOW mesh tree: one task, one queue, acknowledged downlinks.
 *
 * The ESP-NOW receive callback only copies a frame into the queue. Everything
 * else — parent selection, route learning, forwarding, retries, command
 * delivery, reporting — happens on the en2m task, which is also where
 * application callbacks are invoked. Nothing in this layer needs pumping.
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

static void en2m_lock(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }
}

static void en2m_unlock(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
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

/** @return true when the parent was just lost, so the caller can notify. */
static bool en2m_expire_parent(en2m_event_parent_t *out_lost)
{
    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR || !s_ctx.has_parent) {
        return false;
    }
    if ((en2m_now_ms() - s_ctx.parent_last_us / 1000) <= EN2M_PARENT_STALE_MS) {
        return false;
    }

    en2m_mac_copy(out_lost->mac, s_ctx.parent_mac);
    out_lost->cost = s_ctx.path_cost;
    out_lost->rssi = 0;
    s_ctx.has_parent = false;
    s_ctx.path_cost = 255;
    en2m_emit_log("parent stale");
    return true;
}

/** @return true when a new parent was adopted. */
static bool en2m_consider_parent(const uint8_t mac[6], uint8_t role, uint8_t their_cost, int8_t rssi)
{
    uint8_t new_cost;
    bool better = false;
    char log_buf[64];
    char mac_str[18];

    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        return false;
    }
    if (role != EN2M_ROLE_COORDINATOR && role != EN2M_ROLE_ROUTER) {
        return false;
    }
    if (their_cost >= 254) {
        return false;
    }

    new_cost = (uint8_t)(their_cost + 1);
    if (new_cost > EN2M_HOP_LIMIT) {
        return false;
    }

    if (!s_ctx.has_parent) {
        better = true;
    } else if (new_cost < s_ctx.path_cost) {
        better = true;
    } else if (new_cost == s_ctx.path_cost && en2m_mac_equal(s_ctx.parent_mac, mac)) {
        s_ctx.parent_last_us = en2m_now_us();
        return false;
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
        return false;
    }

    en2m_mac_copy(s_ctx.parent_mac, mac);
    s_ctx.has_parent = true;
    s_ctx.path_cost = new_cost;
    s_ctx.parent_last_us = en2m_now_us();
    en2m_add_peer(s_ctx.parent_mac);

    en2m_mac_to_str(s_ctx.parent_mac, mac_str);
    snprintf(log_buf, sizeof(log_buf), "parent=%s cost=%u rssi=%d", mac_str, s_ctx.path_cost, (int)rssi);
    en2m_emit_log(log_buf);
    return true;
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

/* ---- acknowledged downlinks ---- */

/** Caller holds the lock. */
static void en2m_pending_add(const uint8_t dest[6], uint16_t cmd_id, const en2m_pkt_t *pkt)
{
    uint32_t interval = s_ctx.config.retry_interval_ms ? s_ctx.config.retry_interval_ms : EN2M_CMD_RETRY_MS;
    int slot = -1;

    for (int i = 0; i < EN2M_MAX_PENDING; i++) {
        if (s_ctx.pending[i].used && s_ctx.pending[i].cmd_id == cmd_id &&
            en2m_mac_equal(s_ctx.pending[i].dest, dest)) {
            slot = i;
            break;
        }
        if (!s_ctx.pending[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "no free retry slot; command %u is sent unacknowledged", cmd_id);
        return;
    }

    s_ctx.pending[slot].used = true;
    en2m_mac_copy(s_ctx.pending[slot].dest, dest);
    s_ctx.pending[slot].cmd_id = cmd_id;
    s_ctx.pending[slot].attempts = 1;
    s_ctx.pending[slot].next_us = en2m_now_us() + (int64_t)interval * 1000;
    s_ctx.pending[slot].pkt = *pkt;
}

/** Caller holds the lock. @return true when a pending entry was cleared. */
static bool en2m_pending_resolve(const uint8_t origin[6], uint16_t cmd_id, uint8_t *out_attempts)
{
    for (int i = 0; i < EN2M_MAX_PENDING; i++) {
        if (s_ctx.pending[i].used && s_ctx.pending[i].cmd_id == cmd_id &&
            en2m_mac_equal(s_ctx.pending[i].dest, origin)) {
            *out_attempts = s_ctx.pending[i].attempts;
            s_ctx.pending[i].used = false;
            return true;
        }
    }
    return false;
}

/** Resend or expire outstanding downlinks. Caller holds the lock. */
static void en2m_pending_tick(en2m_event_ack_t *timeouts, int *timeout_count, int timeout_max)
{
    uint8_t max_retries = s_ctx.config.max_retries ? s_ctx.config.max_retries : EN2M_CMD_RETRIES;
    uint32_t interval = s_ctx.config.retry_interval_ms ? s_ctx.config.retry_interval_ms : EN2M_CMD_RETRY_MS;
    int64_t now = en2m_now_us();

    for (int i = 0; i < EN2M_MAX_PENDING; i++) {
        en2m_pending_t *p = &s_ctx.pending[i];
        uint8_t next[6];

        if (!p->used || now < p->next_us) {
            continue;
        }
        if (p->attempts > max_retries) {
            p->used = false;
            if (*timeout_count < timeout_max) {
                en2m_mac_copy(timeouts[*timeout_count].mac, p->dest);
                timeouts[*timeout_count].transaction_id = p->cmd_id;
                timeouts[*timeout_count].attempts = p->attempts;
                (*timeout_count)++;
            }
            continue;
        }

        p->attempts++;
        p->next_us = now + (int64_t)interval * 1000;
        if (!en2m_lookup_route(p->dest, next)) {
            en2m_mac_copy(next, p->dest);
        }
        en2m_send_raw(next, &p->pkt);
    }
}

/* ---- receive path ---- */

/**
 * Classify and act on one frame. Runs with the lock held, so anything that
 * calls back into the application is reported through the out-parameters and
 * performed by the caller after unlocking.
 */
typedef struct {
    bool deliver_command;
    bool parent_found;
    bool ack_resolved;
    en2m_event_parent_t parent;
    en2m_event_ack_t ack;
    bool uplink;
    int8_t uplink_rssi;
    uint8_t uplink_from[6];
} en2m_rx_outcome_t;

static void en2m_handle_rx(const uint8_t from[6], const en2m_pkt_t *in, int8_t rssi,
                           en2m_rx_outcome_t *out)
{
    if (in->magic != EN2M_MAGIC || in->version != EN2M_VERSION) {
        return;
    }
    if (en2m_mac_equal(from, s_ctx.self_mac)) {
        return;
    }

    en2m_remember_neighbor(from, in->role, in->cost, rssi);

    if (in->msg_type == EN2M_MSG_BEACON) {
        if (en2m_consider_parent(from, in->role, in->cost, rssi)) {
            out->parent_found = true;
            en2m_mac_copy(out->parent.mac, s_ctx.parent_mac);
            out->parent.cost = s_ctx.path_cost;
            out->parent.rssi = rssi;
        }
        return;
    }

    if (in->msg_type == EN2M_MSG_CMD && en2m_mac_equal(in->dest, s_ctx.self_mac)) {
        out->deliver_command = true;
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

        if (in->msg_type == EN2M_MSG_ACK && in->cmd_id != 0) {
            uint8_t attempts = 0;
            if (en2m_pending_resolve(in->origin, in->cmd_id, &attempts)) {
                out->ack_resolved = true;
                en2m_mac_copy(out->ack.mac, in->origin);
                out->ack.transaction_id = in->cmd_id;
                out->ack.attempts = attempts;
            }
        }

        out->uplink = true;
        out->uplink_rssi = rssi;
        en2m_mac_copy(out->uplink_from, from);
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
    en2m_item_t item = {.kind = EN2M_ITEM_RX};

    if (!s_ctx.inited || info == NULL || data == NULL || len < (int)offsetof(en2m_pkt_t, data)) {
        return;
    }
    if (len > (int)sizeof(en2m_pkt_t)) {
        len = (int)sizeof(en2m_pkt_t);
    }

    en2m_mac_copy(item.u.rx.src, info->src_addr);
    item.u.rx.rssi = (info->rx_ctrl != NULL) ? info->rx_ctrl->rssi : 0;
    memcpy(&item.u.rx.pkt, data, (size_t)len);

    if (s_ctx.queue == NULL || xQueueSend(s_ctx.queue, &item, 0) != pdTRUE) {
        s_ctx.rx_dropped++;
    }
}

/* ---- the en2m task ---- */

esp_err_t en2m_task_post(const en2m_item_t *item, bool from_isr, BaseType_t *higher_prio_task_woken)
{
    BaseType_t ok;

    if (item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (from_isr) {
        ok = xQueueSendFromISR(s_ctx.queue, item, higher_prio_task_woken);
    } else {
        ok = xQueueSend(s_ctx.queue, item, 0);
    }
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool en2m_task_running(void)
{
    return s_ctx.running;
}

bool en2m_task_is_current(void)
{
    return s_ctx.task != NULL && s_ctx.task == xTaskGetCurrentTaskHandle();
}

esp_err_t en2m_schedule(en2m_work_fn_t fn, void *arg)
{
    en2m_item_t item = {.kind = EN2M_ITEM_WORK};

    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    item.u.work.fn = fn;
    item.u.work.arg = arg;
    return en2m_task_post(&item, false, NULL);
}

esp_err_t en2m_schedule_from_isr(en2m_work_fn_t fn, void *arg, BaseType_t *higher_prio_task_woken)
{
    en2m_item_t item = {.kind = EN2M_ITEM_WORK};

    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    item.u.work.fn = fn;
    item.u.work.arg = arg;
    return en2m_task_post(&item, true, higher_prio_task_woken);
}

static void en2m_dispatch_rx(const en2m_item_t *item)
{
    en2m_rx_outcome_t outcome = {0};

    en2m_lock();
    en2m_handle_rx(item->u.rx.src, &item->u.rx.pkt, item->u.rx.rssi, &outcome);
    en2m_unlock();

    if (outcome.parent_found) {
        en2m_event_post(EN2M_EVENT_PARENT_FOUND, &outcome.parent, sizeof(outcome.parent));
        en2m_model_on_link_change(true);
    }
    if (outcome.ack_resolved) {
        en2m_event_post(EN2M_EVENT_ACK_RECEIVED, &outcome.ack, sizeof(outcome.ack));
    }
    if (outcome.uplink && s_ctx.config.on_uplink != NULL) {
        s_ctx.config.on_uplink(&item->u.rx.pkt, outcome.uplink_rssi, outcome.uplink_from,
                               s_ctx.config.user_ctx);
    }
    if (outcome.deliver_command) {
        if (s_ctx.config.on_command != NULL) {
            s_ctx.config.on_command(&item->u.rx.pkt, s_ctx.config.user_ctx);
        } else {
            en2m_model_on_command_frame(&item->u.rx.pkt);
        }
    }
}

static void en2m_maintenance(int64_t now_ms)
{
    en2m_event_ack_t timeouts[EN2M_MAX_PENDING];
    en2m_event_parent_t lost = {0};
    en2m_event_dropped_t dropped = {0};
    int timeout_count = 0;
    bool parent_lost;
    bool beacon_due = false;
    bool hello_due = false;

    en2m_lock();
    parent_lost = en2m_expire_parent(&lost);
    en2m_expire_routes();
    en2m_pending_tick(timeouts, &timeout_count, EN2M_MAX_PENDING);

    if (s_ctx.config.role != EN2M_ROLE_LEAF &&
        (now_ms - s_ctx.last_beacon_us / 1000) >= EN2M_BEACON_MS_DEFAULT) {
        s_ctx.last_beacon_us = now_ms * 1000;
        beacon_due = true;
    }
    if (s_ctx.config.role != EN2M_ROLE_COORDINATOR &&
        (now_ms - s_ctx.last_hello_us / 1000) >= EN2M_HEARTBEAT_MS) {
        s_ctx.last_hello_us = now_ms * 1000;
        hello_due = true;
    }
    if (s_ctx.rx_dropped != 0) {
        dropped.total = s_ctx.rx_dropped;
    }
    if (beacon_due) {
        en2m_send_beacon();
    }
    en2m_unlock();

    if (hello_due) {
        en2m_send_uplink(EN2M_MSG_HEARTBEAT, 0, NULL, 0);
    }
    if (parent_lost) {
        en2m_event_post(EN2M_EVENT_PARENT_LOST, &lost, sizeof(lost));
        en2m_model_on_link_change(false);
    }
    for (int i = 0; i < timeout_count; i++) {
        char mac_str[18];
        en2m_mac_to_str(timeouts[i].mac, mac_str);
        ESP_LOGW(TAG, "command %u to %s was never acknowledged", timeouts[i].transaction_id, mac_str);
        en2m_event_post(EN2M_EVENT_ACK_TIMEOUT, &timeouts[i], sizeof(timeouts[i]));
    }
    if (dropped.total != 0) {
        s_ctx.rx_dropped = 0;
        ESP_LOGW(TAG, "dropped %u frames, the queue could not keep up", (unsigned)dropped.total);
        en2m_event_post(EN2M_EVENT_RX_DROPPED, &dropped, sizeof(dropped));
    }
}

static void en2m_task(void *arg)
{
    en2m_item_t item;

    (void)arg;
    s_ctx.last_tick_ms = en2m_now_ms();

    while (s_ctx.running) {
        if (xQueueReceive(s_ctx.queue, &item, pdMS_TO_TICKS(EN2M_TICK_MS)) == pdTRUE) {
            switch (item.kind) {
            case EN2M_ITEM_RX:
                en2m_dispatch_rx(&item);
                break;
            case EN2M_ITEM_WORK:
                item.u.work.fn(item.u.work.arg);
                break;
            case EN2M_ITEM_ATTR:
                en2m_attribute_set(item.u.attr.path.endpoint_id, item.u.attr.path.cluster_id,
                                   item.u.attr.path.attribute_id, item.u.attr.value);
                break;
            }
        }

        int64_t now_ms = en2m_now_ms();
        if (now_ms - s_ctx.last_tick_ms >= EN2M_TICK_MS) {
            s_ctx.last_tick_ms = now_ms;
            en2m_maintenance(now_ms);
            en2m_model_tick(now_ms);
        }
    }

    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

/* ---- bring-up ---- */

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
    uint32_t stack = 4096;
    uint8_t priority = 5;
    esp_err_t err;
    uint8_t bcast[6];

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->role == EN2M_ROLE_COORDINATOR || config->role == EN2M_ROLE_ROUTER ||
                            config->role == EN2M_ROLE_LEAF,
                        ESP_ERR_INVALID_ARG, TAG, "invalid role");
    ESP_RETURN_ON_FALSE(!s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = *config;
    if (s_ctx.config.channel == 0) {
        s_ctx.config.channel = EN2M_WIFI_CHANNEL;
    }
    strncpy(s_ctx.name, (config->name != NULL) ? config->name : EN2M_DEVICE_NAME, sizeof(s_ctx.name) - 1);
    s_ctx.path_cost = 255;
    s_ctx.next_cmd_id = 1;

    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    s_ctx.queue = xQueueCreate(EN2M_QUEUE_LEN, sizeof(en2m_item_t));
    ESP_RETURN_ON_FALSE(s_ctx.queue != NULL, ESP_ERR_NO_MEM, TAG, "queue alloc failed");

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
    s_ctx.running = true;

    if (en2m_model_config() != NULL) {
        if (en2m_model_config()->task_stack_size != 0) {
            stack = en2m_model_config()->task_stack_size;
        }
        if (en2m_model_config()->task_priority != 0) {
            priority = en2m_model_config()->task_priority;
        }
    }
    if (xTaskCreate(en2m_task, "en2m", stack, NULL, priority, &s_ctx.task) != pdPASS) {
        s_ctx.running = false;
        s_ctx.inited = false;
        ESP_LOGE(TAG, "could not create the en2m task");
        return ESP_ERR_NO_MEM;
    }

    en2m_emit_log("mesh init");
    return ESP_OK;
}

void en2m_mesh_deinit(void)
{
    if (!s_ctx.inited) {
        return;
    }

    s_ctx.running = false;
    for (int i = 0; i < 20 && s_ctx.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(EN2M_TICK_MS / 2));
    }

    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_ctx.queue != NULL) {
        vQueueDelete(s_ctx.queue);
        s_ctx.queue = NULL;
    }
    if (s_ctx.lock != NULL) {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }
    s_ctx.inited = false;
}

void en2m_mesh_loop(void)
{
    static bool warned;

    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "en2m_mesh_loop() is obsolete: the transport runs its own task");
    }
}

/* ---- transmit ---- */

esp_err_t en2m_send_uplink(uint8_t msg_type, uint16_t cmd_id, const uint8_t *data, uint8_t len)
{
    en2m_pkt_t pkt;
    uint8_t bcast[6];
    esp_err_t err;

    if (s_ctx.config.role == EN2M_ROLE_COORDINATOR) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_ctx.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    en2m_lock();
    if (!s_ctx.has_parent && msg_type != EN2M_MSG_HELLO && msg_type != EN2M_MSG_HEARTBEAT) {
        en2m_unlock();
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
        err = en2m_send_raw(s_ctx.parent_mac, &pkt);
    } else {
        en2m_mac_broadcast(bcast);
        err = en2m_send_raw(bcast, &pkt);
    }
    en2m_unlock();
    return err;
}

uint16_t en2m_next_cmd_id(void)
{
    uint16_t id;

    en2m_lock();
    if (s_ctx.next_cmd_id == 0) {
        s_ctx.next_cmd_id = 1;
    }
    id = s_ctx.next_cmd_id++;
    en2m_unlock();
    return id;
}

esp_err_t en2m_send_downlink(const uint8_t dest_mac[6], uint16_t cmd_id, const uint8_t *data, uint8_t len)
{
    en2m_pkt_t pkt;
    uint8_t next[6];
    esp_err_t err;

    ESP_RETURN_ON_FALSE(dest_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "dest_mac is NULL");
    if (s_ctx.config.role != EN2M_ROLE_COORDINATOR) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_ctx.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    en2m_lock();
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

    if (!en2m_lookup_route(dest_mac, next)) {
        en2m_mac_copy(next, dest_mac);
    }
    err = en2m_send_raw(next, &pkt);
    if (err == ESP_OK && cmd_id != 0) {
        en2m_pending_add(dest_mac, cmd_id, &pkt);
    }
    en2m_unlock();
    return err;
}

/* ---- state accessors ---- */

void en2m_set_pairing(bool enabled)
{
    en2m_event_pairing_t event = {.enabled = enabled};
    bool changed;

    en2m_lock();
    changed = (s_ctx.pairing != enabled);
    s_ctx.pairing = enabled;
    en2m_unlock();

    if (changed) {
        en2m_event_post(EN2M_EVENT_PAIRING_CHANGED, &event, sizeof(event));
    }
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
    en2m_lock();
    if (s_ctx.has_parent) {
        en2m_mac_copy(out, s_ctx.parent_mac);
    } else {
        en2m_mac_broadcast(out);
    }
    en2m_unlock();
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
    en2m_lock();
    strncpy(s_ctx.name, name, sizeof(s_ctx.name) - 1);
    s_ctx.name[sizeof(s_ctx.name) - 1] = '\0';
    en2m_unlock();
}
