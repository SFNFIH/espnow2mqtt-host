#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "en2m_mesh.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_host_link.h"

static const char *TAG = "coord";

typedef struct {
    bool used;
    uint8_t mac[6];
    char model[12];
    char name[16];
    uint8_t role;
    uint8_t hop;
    int8_t rssi;
    uint8_t via[6];
    int64_t last_ms;
} host_peer_t;

static host_peer_t s_peers[EN2M_MAX_ROUTES];
static int64_t s_pair_until_ms;

static int64_t millis(void)
{
    return esp_timer_get_time() / 1000;
}

static void usb_emit_json(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        usb_host_link_write(s);
        cJSON_free(s);
    }
}

static void usb_log(const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "log");
    cJSON_AddStringToObject(o, "msg", msg);
    usb_emit_json(o);
    cJSON_Delete(o);
}

static int find_peer(const uint8_t mac[6])
{
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (s_peers[i].used && en2m_mac_equal(s_peers[i].mac, mac)) {
            return i;
        }
    }
    return -1;
}

static int alloc_peer(const uint8_t mac[6])
{
    int idx = find_peer(mac);
    if (idx >= 0) {
        return idx;
    }
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (!s_peers[i].used) {
            memset(&s_peers[i], 0, sizeof(s_peers[i]));
            en2m_mac_copy(s_peers[i].mac, mac);
            s_peers[i].used = true;
            return i;
        }
    }
    return -1;
}

static void emit_hello(void)
{
    uint8_t mac[6];
    char macs[18];
    en2m_self_mac(mac);
    en2m_mac_to_str(mac, macs);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hello");
    cJSON_AddNumberToObject(o, "version", EN2M_VERSION);
    cJSON_AddStringToObject(o, "role", "coordinator");
    cJSON_AddStringToObject(o, "mac", macs);
    cJSON_AddStringToObject(o, "fw", EN2M_FW_VERSION);
    cJSON_AddNumberToObject(o, "channel", EN2M_WIFI_CHANNEL);
    cJSON_AddBoolToObject(o, "mesh", true);
    cJSON_AddStringToObject(o, "stack", "esp-idf");
    usb_emit_json(o);
    cJSON_Delete(o);
}

static void emit_device(const host_peer_t *p, const char *event)
{
    char macs[18], vias[18];
    en2m_mac_to_str(p->mac, macs);
    en2m_mac_to_str(p->via, vias);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "device");
    cJSON_AddStringToObject(o, "event", event);
    cJSON_AddStringToObject(o, "mac", macs);
    if (strcmp(event, "offline") != 0) {
        if (p->model[0]) {
            cJSON_AddStringToObject(o, "model", p->model);
        }
        if (p->name[0]) {
            cJSON_AddStringToObject(o, "name", p->name);
        }
        cJSON_AddNumberToObject(o, "rssi", p->rssi);
        cJSON_AddNumberToObject(o, "hop", p->hop);
        cJSON_AddStringToObject(o, "via", vias);
        if (p->role == EN2M_ROLE_ROUTER) {
            cJSON_AddStringToObject(o, "node_role", "router");
        } else if (p->role == EN2M_ROLE_LEAF) {
            cJSON_AddStringToObject(o, "node_role", "leaf");
        } else {
            cJSON_AddStringToObject(o, "node_role", "unknown");
        }
    }
    usb_emit_json(o);
    cJSON_Delete(o);
}

static void on_uplink(const en2m_pkt_t *pkt, int8_t rssi, const uint8_t from_mac[6], void *user)
{
    (void)user;
    int idx = alloc_peer(pkt->origin);
    if (idx < 0) {
        usb_log("peer table full");
        return;
    }
    host_peer_t *p = &s_peers[idx];
    bool first = (p->last_ms == 0);
    p->last_ms = millis();
    p->rssi = rssi;
    p->hop = pkt->hop;
    p->role = pkt->role;
    en2m_mac_copy(p->via, from_mac);
    if (pkt->model[0]) {
        strncpy(p->model, pkt->model, sizeof(p->model) - 1);
    }
    if (pkt->name[0]) {
        strncpy(p->name, pkt->name, sizeof(p->name) - 1);
    }

    if (pkt->msg_type == EN2M_MSG_HELLO || pkt->msg_type == EN2M_MSG_HEARTBEAT) {
        emit_device(p, first ? "online" : "info");
    }

    if (pkt->msg_type == EN2M_MSG_STATE || pkt->msg_type == EN2M_MSG_HELLO ||
        pkt->msg_type == EN2M_MSG_ACK) {
        char macs[18], vias[18];
        en2m_mac_to_str(pkt->origin, macs);
        en2m_mac_to_str(from_mac, vias);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", pkt->msg_type == EN2M_MSG_ACK ? "ack" : "state");
        cJSON_AddStringToObject(o, "mac", macs);
        cJSON_AddNumberToObject(o, "ts", (double)millis());
        cJSON_AddNumberToObject(o, "hop", pkt->hop);
        cJSON_AddStringToObject(o, "via", vias);
        if (pkt->msg_type == EN2M_MSG_ACK) {
            cJSON_AddNumberToObject(o, "id", pkt->cmd_id);
            cJSON_AddBoolToObject(o, "ok", true);
        }
        if (pkt->data_len) {
            char tmp[EN2M_DATA_MAX + 1];
            memcpy(tmp, pkt->data, pkt->data_len);
            tmp[pkt->data_len] = 0;
            cJSON *payload = cJSON_Parse(tmp);
            if (payload) {
                cJSON_AddItemToObject(o, "payload", payload);
            } else {
                cJSON *wrap = cJSON_CreateObject();
                cJSON_AddStringToObject(wrap, "raw", tmp);
                cJSON_AddItemToObject(o, "payload", wrap);
            }
        }
        usb_emit_json(o);
        cJSON_Delete(o);
    }
}

static void on_mesh_log(const char *msg, void *user)
{
    (void)user;
    usb_log(msg);
}

static void handle_host_line(const char *line, void *user)
{
    (void)user;
    cJSON *doc = cJSON_Parse(line);
    if (!doc) {
        usb_log("bad json");
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(doc, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(doc);
        usb_log("missing type");
        return;
    }

    if (strcmp(type->valuestring, "ping") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "pong");
        cJSON_AddNumberToObject(o, "ms", (double)millis());
        usb_emit_json(o);
        cJSON_Delete(o);
    } else if (strcmp(type->valuestring, "pair") == 0) {
        int seconds = 60;
        const cJSON *sec = cJSON_GetObjectItem(doc, "seconds");
        if (cJSON_IsNumber(sec)) {
            seconds = sec->valueint;
        }
        if (seconds < 1) {
            seconds = 1;
        }
        if (seconds > 300) {
            seconds = 300;
        }
        en2m_set_pairing(true);
        s_pair_until_ms = millis() + seconds * 1000LL;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "log");
        cJSON_AddStringToObject(o, "msg", "pairing_enabled");
        cJSON_AddNumberToObject(o, "seconds", seconds);
        usb_emit_json(o);
        cJSON_Delete(o);
    } else if (strcmp(type->valuestring, "list") == 0) {
        for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
            if (s_peers[i].used) {
                emit_device(&s_peers[i], "info");
            }
        }
    } else if (strcmp(type->valuestring, "unpair") == 0) {
        const cJSON *macj = cJSON_GetObjectItem(doc, "mac");
        uint8_t mac[6];
        if (!cJSON_IsString(macj) || !en2m_mac_from_str(macj->valuestring, mac)) {
            usb_log("bad mac");
        } else {
            int idx = find_peer(mac);
            if (idx >= 0) {
                emit_device(&s_peers[idx], "offline");
                s_peers[idx].used = false;
            }
            en2m_forget_route(mac);
        }
    } else if (strcmp(type->valuestring, "cmd") == 0) {
        const cJSON *macj = cJSON_GetObjectItem(doc, "mac");
        const cJSON *idj = cJSON_GetObjectItem(doc, "id");
        const cJSON *payload = cJSON_GetObjectItem(doc, "payload");
        uint8_t mac[6];
        if (!cJSON_IsString(macj) || !en2m_mac_from_str(macj->valuestring, mac)) {
            usb_log("bad mac");
        } else {
            uint16_t id = cJSON_IsNumber(idj) ? (uint16_t)idj->valueint : 0;
            char *ps = payload ? cJSON_PrintUnformatted(payload) : NULL;
            esp_err_t err = en2m_send_downlink(
                mac, id, (const uint8_t *)(ps ? ps : "{}"),
                (uint8_t)strnlen(ps ? ps : "{}", EN2M_DATA_MAX));
            if (ps) {
                cJSON_free(ps);
            }
            if (err != ESP_OK) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "type", "ack");
                cJSON_AddStringToObject(o, "mac", macj->valuestring);
                cJSON_AddNumberToObject(o, "id", id);
                cJSON_AddBoolToObject(o, "ok", false);
                cJSON_AddStringToObject(o, "error", "send_fail");
                usb_emit_json(o);
                cJSON_Delete(o);
            }
        }
    } else {
        usb_log("unknown cmd");
    }

    cJSON_Delete(doc);
}

static void mesh_task(void *arg)
{
    (void)arg;
    int64_t last_check = 0;
    int64_t last_hello = 0;
    while (1) {
        en2m_mesh_loop();

        if (en2m_pairing() && millis() >= s_pair_until_ms) {
            en2m_set_pairing(false);
            usb_log("pairing_disabled");
        }

        int64_t now = millis();
        if (now - last_check > 2000) {
            last_check = now;
            for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
                if (!s_peers[i].used) {
                    continue;
                }
                if (now - s_peers[i].last_ms > EN2M_OFFLINE_MS) {
                    emit_device(&s_peers[i], "offline");
                    s_peers[i].used = false;
                }
            }
        }
        if (now - last_hello > 30000) {
            last_hello = now;
            emit_hello();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    memset(s_peers, 0, sizeof(s_peers));
    ESP_LOGI(TAG, "espnow2mqtt coordinator (ESP-IDF)");

    ESP_ERROR_CHECK(usb_host_link_start(handle_host_line, NULL));

    en2m_app_config_t cfg = {
        .role = EN2M_ROLE_COORDINATOR,
        .model = "s3-coord",
        .name = "coordinator",
        .fw = EN2M_FW_VERSION,
        .channel = EN2M_WIFI_CHANNEL,
        .on_uplink = on_uplink,
        .on_log = on_mesh_log,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(en2m_mesh_init(&cfg));

    emit_hello();
    usb_log("coordinator ready (esp-idf mesh)");

    xTaskCreate(mesh_task, "mesh", 8192, NULL, 4, NULL);
}
