/**
 * @file en2m_model.c
 * @brief Lifecycle, command decoding and state reporting.
 *
 * Everything here runs on the en2m task: command dispatch, the read refresh
 * that feeds pull-style sensors, report scheduling and the NVS write-back.
 * Applications only supply callbacks.
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "en2m_priv.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "en2m_model";

#define EN2M_REPORT_INTERVAL_LEAF_MS 30000
#define EN2M_REPORT_INTERVAL_MAINS_MS 15000
#define EN2M_MIN_REPORT_INTERVAL_MS 1000
#define EN2M_PERSIST_FLUSH_MS 5000

static struct {
    bool configured;
    bool started;
    en2m_device_config_t cfg;
    int64_t last_report_ms;
    int64_t next_report_ms; /* 0 = nothing scheduled */
    int64_t last_persist_ms;
    int64_t next_identify_ms;
} s_model;

static int64_t en2m_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

const en2m_device_config_t *en2m_model_config(void)
{
    return s_model.configured ? &s_model.cfg : NULL;
}

bool en2m_is_started(void)
{
    return s_model.started;
}

/* ---- textual value mappings shared with the host bridge ---- */

static const char *en2m_fan_mode_str(uint8_t mode)
{
    switch (mode) {
    case EN2M_FAN_LOW:
        return "low";
    case EN2M_FAN_MEDIUM:
        return "medium";
    case EN2M_FAN_HIGH:
        return "high";
    case EN2M_FAN_ON:
        return "on";
    case EN2M_FAN_AUTO:
        return "auto";
    case EN2M_FAN_SMART:
        return "smart";
    case EN2M_FAN_OFF:
    default:
        return "off";
    }
}

static en2m_fan_mode_t en2m_fan_mode_parse(const char *s)
{
    if (s == NULL) {
        return EN2M_FAN_OFF;
    }
    if (strcmp(s, "low") == 0) {
        return EN2M_FAN_LOW;
    }
    if (strcmp(s, "medium") == 0 || strcmp(s, "med") == 0) {
        return EN2M_FAN_MEDIUM;
    }
    if (strcmp(s, "high") == 0) {
        return EN2M_FAN_HIGH;
    }
    if (strcmp(s, "on") == 0) {
        return EN2M_FAN_ON;
    }
    if (strcmp(s, "auto") == 0) {
        return EN2M_FAN_AUTO;
    }
    if (strcmp(s, "smart") == 0) {
        return EN2M_FAN_SMART;
    }
    return EN2M_FAN_OFF;
}

static const char *en2m_hvac_mode_str(uint8_t mode)
{
    switch (mode) {
    case EN2M_THERMOSTAT_AUTO:
        return "auto";
    case EN2M_THERMOSTAT_COOL:
        return "cool";
    case EN2M_THERMOSTAT_HEAT:
        return "heat";
    case EN2M_THERMOSTAT_FAN_ONLY:
        return "fan_only";
    case EN2M_THERMOSTAT_OFF:
    default:
        return "off";
    }
}

static en2m_thermostat_mode_t en2m_hvac_mode_parse(const char *s)
{
    if (s == NULL) {
        return EN2M_THERMOSTAT_OFF;
    }
    if (strcmp(s, "auto") == 0) {
        return EN2M_THERMOSTAT_AUTO;
    }
    if (strcmp(s, "cool") == 0) {
        return EN2M_THERMOSTAT_COOL;
    }
    if (strcmp(s, "heat") == 0) {
        return EN2M_THERMOSTAT_HEAT;
    }
    if (strcmp(s, "fan_only") == 0 || strcmp(s, "fan") == 0) {
        return EN2M_THERMOSTAT_FAN_ONLY;
    }
    return EN2M_THERMOSTAT_OFF;
}

/* ---- data model helpers ---- */

static bool en2m_model_read(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id, int64_t *out)
{
    en2m_value_t value;

    if (en2m_attribute_get(endpoint_id, cluster_id, attribute_id, &value) != ESP_OK) {
        return false;
    }
    *out = en2m_value_as_int(&value);
    return true;
}

/** Endpoint id of the lowest-numbered endpoint carrying @p cluster_id, or 0. */
static uint8_t en2m_model_endpoint_with(uint16_t cluster_id)
{
    uint8_t found = 0;

    en2m_dm_lock();
    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        struct en2m_endpoint *ep = en2m_dm_endpoint_slot(i);
        if (ep == NULL || !ep->used) {
            continue;
        }
        for (int c = 0; c < EN2M_MAX_CLUSTERS_PER_ENDPOINT; c++) {
            if (ep->clusters[c].used && ep->clusters[c].id == cluster_id) {
                if (found == 0 || ep->id < found) {
                    found = ep->id;
                }
                break;
            }
        }
    }
    en2m_dm_unlock();
    return found;
}

/* ---- report serialization ---- */

static void en2m_caps_add(cJSON *caps, const char *name)
{
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, caps)
    {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0) {
            return;
        }
    }
    cJSON_AddItemToArray(caps, cJSON_CreateString(name));
}

/**
 * Serialize one cluster into the flat key space the MQTT bridge and the Home
 * Assistant integration consume. Keys are global, so the lowest endpoint that
 * owns a cluster wins; multi-endpoint nodes should use distinct clusters.
 */
static void en2m_report_cluster(cJSON *root, cJSON *caps, uint8_t ep_id, uint16_t cluster_id)
{
    int64_t v = 0;
    int64_t v2 = 0;

    switch (cluster_id) {
    case EN2M_CLUSTER_ON_OFF:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_ON_OFF, &v)) {
            cJSON_AddStringToObject(root, "switch", v ? "ON" : "OFF");
            en2m_caps_add(caps, "switch");
        }
        break;

    case EN2M_CLUSTER_LEVEL_CONTROL:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_CURRENT_LEVEL, &v)) {
            cJSON_AddNumberToObject(root, "brightness", (double)v);
            en2m_caps_add(caps, "light");
        }
        break;

    case EN2M_CLUSTER_COLOR_CONTROL:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_COLOR_TEMPERATURE_MIREDS, &v)) {
            cJSON_AddNumberToObject(root, "color_temp", (double)v);
            cJSON_AddStringToObject(root, "color_mode", "color_temp");
            en2m_caps_add(caps, "light");
        }
        break;

    case EN2M_CLUSTER_BOOLEAN_STATE:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_STATE_VALUE, &v)) {
            cJSON_AddStringToObject(root, "contact", v ? "ON" : "OFF");
            en2m_caps_add(caps, "contact");
        }
        break;

    case EN2M_CLUSTER_OCCUPANCY:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_OCCUPANCY, &v)) {
            cJSON_AddStringToObject(root, "occupancy", v ? "ON" : "OFF");
            en2m_caps_add(caps, "occupancy");
        }
        break;

    case EN2M_CLUSTER_ILLUMINANCE:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_MEASURED_VALUE, &v)) {
            cJSON_AddNumberToObject(root, "illuminance", (double)v);
            en2m_caps_add(caps, "illuminance");
        }
        break;

    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_MEASURED_VALUE, &v)) {
            cJSON_AddNumberToObject(root, "temperature", v / 100.0);
            en2m_caps_add(caps, "temperature");
        }
        break;

    case EN2M_CLUSTER_RELATIVE_HUMIDITY:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_MEASURED_VALUE, &v)) {
            cJSON_AddNumberToObject(root, "humidity", v / 100.0);
            en2m_caps_add(caps, "humidity");
        }
        break;

    case EN2M_CLUSTER_PRESSURE_MEASUREMENT:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_MEASURED_VALUE, &v)) {
            cJSON_AddNumberToObject(root, "pressure", v / 10.0);
            en2m_caps_add(caps, "pressure");
        }
        break;

    case EN2M_CLUSTER_SMOKE_CO:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_SMOKE_STATE, &v)) {
            cJSON_AddStringToObject(root, "smoke", v ? "ON" : "OFF");
            en2m_caps_add(caps, "smoke");
        }
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_CO_STATE, &v)) {
            cJSON_AddStringToObject(root, "carbon_monoxide", v ? "ON" : "OFF");
            en2m_caps_add(caps, "carbon_monoxide");
        }
        break;

    case EN2M_CLUSTER_ELECTRICAL_POWER:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_ACTIVE_POWER_MW, &v)) {
            cJSON_AddNumberToObject(root, "power", v / 1000.0);
            en2m_caps_add(caps, "power");
        }
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_ENERGY_MWH, &v)) {
            cJSON_AddNumberToObject(root, "energy", v / 1000.0);
            en2m_caps_add(caps, "energy");
        }
        break;

    case EN2M_CLUSTER_FAN_CONTROL:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_FAN_MODE, &v)) {
            cJSON_AddStringToObject(root, "fan_mode", en2m_fan_mode_str((uint8_t)v));
            en2m_caps_add(caps, "fan");
        }
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_PERCENT_SETTING, &v)) {
            cJSON_AddNumberToObject(root, "percentage", (double)v);
        }
        break;

    case EN2M_CLUSTER_WINDOW_COVERING:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT, &v)) {
            cJSON_AddNumberToObject(root, "position", (double)v);
            cJSON_AddStringToObject(root, "cover", v >= 95 ? "CLOSED" : "OPEN");
            en2m_caps_add(caps, "cover");
        }
        break;

    case EN2M_CLUSTER_DOOR_LOCK:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_LOCK_STATE, &v)) {
            cJSON_AddStringToObject(root, "lock", v == EN2M_LOCK_LOCKED ? "LOCKED" : "UNLOCKED");
            en2m_caps_add(caps, "lock");
        }
        break;

    case EN2M_CLUSTER_THERMOSTAT:
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_SYSTEM_MODE, &v)) {
            cJSON_AddStringToObject(root, "hvac_mode", en2m_hvac_mode_str((uint8_t)v));
            en2m_caps_add(caps, "climate");
        }
        if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_LOCAL_TEMPERATURE, &v2)) {
            cJSON_AddNumberToObject(root, "current_temperature", v2 / 100.0);
        }
        /* Report the setpoint that the active mode is actually chasing. */
        if (en2m_model_read(ep_id, cluster_id,
                            (v == EN2M_THERMOSTAT_COOL) ? EN2M_ATTR_OCCUPIED_COOLING_SETPOINT
                                                        : EN2M_ATTR_OCCUPIED_HEATING_SETPOINT,
                            &v2)) {
            cJSON_AddNumberToObject(root, "target_temperature", v2 / 100.0);
        }
        break;

    default:
        break;
    }
}

/** Assemble the uplink report. @p caps and @p diagnostics trade size for detail. */
static char *en2m_report_build(bool with_caps, bool with_diagnostics)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *caps;
    char *printed;
    bool seen[EN2M_MAX_CLUSTERS_PER_ENDPOINT * EN2M_MAX_ENDPOINTS] = {false};
    uint16_t seen_ids[EN2M_MAX_CLUSTERS_PER_ENDPOINT * EN2M_MAX_ENDPOINTS] = {0};
    int seen_count = 0;

    if (root == NULL) {
        return NULL;
    }
    caps = cJSON_CreateArray();
    if (caps == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    if (with_diagnostics) {
        const char *role = "leaf";
        if (en2m_get_role() == EN2M_ROLE_ROUTER) {
            role = "router";
        } else if (en2m_get_role() == EN2M_ROLE_COORDINATOR) {
            role = "coordinator";
        }
        cJSON_AddStringToObject(root, "node_role", role);
    }

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        struct en2m_endpoint *ep = en2m_dm_endpoint_slot(i);
        if (ep == NULL || !ep->used) {
            continue;
        }
        for (int c = 0; c < EN2M_MAX_CLUSTERS_PER_ENDPOINT; c++) {
            uint16_t cluster_id;
            bool duplicate = false;

            if (!ep->clusters[c].used) {
                continue;
            }
            cluster_id = ep->clusters[c].id;
            for (int s = 0; s < seen_count; s++) {
                if (seen[s] && seen_ids[s] == cluster_id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            seen[seen_count] = true;
            seen_ids[seen_count] = cluster_id;
            seen_count++;
            en2m_report_cluster(root, caps, ep->id, cluster_id);
        }
    }

    /* A dimmable or tunable node is a light in Home Assistant, not a switch. */
    if (cJSON_GetObjectItem(root, "brightness") != NULL || cJSON_GetObjectItem(root, "color_temp") != NULL) {
        cJSON *item = NULL;
        int index = 0;
        cJSON_ArrayForEach(item, caps)
        {
            if (cJSON_IsString(item) && strcmp(item->valuestring, "switch") == 0) {
                cJSON_DeleteItemFromArray(caps, index);
                break;
            }
            index++;
        }
    }

    if (with_caps && cJSON_GetArraySize(caps) > 0) {
        cJSON_AddItemToObject(root, "caps", caps);
    } else {
        cJSON_Delete(caps);
    }

    printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return printed;
}

static esp_err_t en2m_report_transmit(void)
{
    static const struct {
        bool caps;
        bool diagnostics;
    } levels[] = {{true, true}, {true, false}, {false, false}};
    en2m_event_report_t event = {0};
    char *json = NULL;
    size_t len = 0;
    esp_err_t err;

    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        json = en2m_report_build(levels[i].caps, levels[i].diagnostics);
        if (json == NULL) {
            return ESP_ERR_NO_MEM;
        }
        len = strlen(json);
        if (len <= EN2M_DATA_MAX) {
            break;
        }
        cJSON_free(json);
        json = NULL;
    }

    if (json == NULL) {
        /* Even the bare value set does not fit; send what we can and say so. */
        json = en2m_report_build(false, false);
        if (json == NULL) {
            return ESP_ERR_NO_MEM;
        }
        len = EN2M_DATA_MAX;
        event.truncated = true;
        ESP_LOGE(TAG, "report exceeds %d bytes; split the device across endpoints or trim clusters",
                 EN2M_DATA_MAX);
    }

    err = en2m_send_uplink(EN2M_MSG_STATE, 0, (const uint8_t *)json, (uint8_t)len);
    cJSON_free(json);

    s_model.last_report_ms = en2m_now_ms();
    s_model.next_report_ms = 0;

    event.length = (uint16_t)len;
    event.err = err;
    en2m_event_post(EN2M_EVENT_REPORT_SENT, &event, sizeof(event));
    return err;
}

esp_err_t en2m_report_now(void)
{
    ESP_RETURN_ON_FALSE(s_model.started, ESP_ERR_INVALID_STATE, TAG, "en2m not started");

    /* Keep read callbacks and serialization on the en2m task. */
    if (!en2m_task_is_current()) {
        return en2m_report_schedule(0);
    }
    en2m_dm_refresh();
    return en2m_report_transmit();
}

esp_err_t en2m_report_schedule(uint32_t delay_ms)
{
    int64_t now = en2m_now_ms();
    int64_t floor_ms;
    int64_t when;

    if (!s_model.started) {
        return ESP_ERR_INVALID_STATE;
    }

    floor_ms = s_model.last_report_ms + s_model.cfg.min_report_interval_ms;
    when = now + (int64_t)delay_ms;
    if (when < floor_ms) {
        when = floor_ms;
    }
    if (s_model.next_report_ms == 0 || when < s_model.next_report_ms) {
        s_model.next_report_ms = when;
    }
    return ESP_OK;
}

/* ---- change notification ---- */

void en2m_model_on_change(const en2m_attr_path_t *path, const en2m_value_t *value)
{
    en2m_event_attribute_t event = {.path = *path, .value = *value};

    en2m_event_post(EN2M_EVENT_ATTRIBUTE_UPDATED, &event, sizeof(event));

    if (s_model.configured && s_model.cfg.attribute_changed != NULL) {
        s_model.cfg.attribute_changed(path, value, s_model.cfg.user_ctx);
    }
    if (s_model.started && (s_model.cfg.report_mode == EN2M_REPORT_DEFAULT ||
                            s_model.cfg.report_mode == EN2M_REPORT_ON_CHANGE_ONLY)) {
        en2m_report_schedule(0);
    }
}

void en2m_model_on_link_change(bool has_parent)
{
    if (has_parent && s_model.started) {
        s_model.next_report_ms = en2m_now_ms() + 200;
    }
}

/* ---- command handling ---- */

/**
 * Run one decoded command: application handlers first, then the built-in
 * translation into attribute writes.
 */
static esp_err_t en2m_model_exec(uint8_t endpoint_id, uint16_t cluster_id, en2m_command_id_t id,
                                 const char *name, en2m_value_t arg, uint16_t transaction_id,
                                 const char *json)
{
    en2m_command_t cmd = {
        .endpoint_id = endpoint_id,
        .cluster_id = cluster_id,
        .id = id,
        .name = (name != NULL) ? name : "",
        .arg = arg,
        .transaction_id = transaction_id,
        .json = json,
    };
    en2m_event_command_t event = {
        .endpoint_id = endpoint_id,
        .cluster_id = cluster_id,
        .id = id,
        .transaction_id = transaction_id,
    };
    en2m_command_handler_t cluster_cb = NULL;
    void *cluster_ctx = NULL;
    struct en2m_cluster *cluster;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    if (endpoint_id == 0) {
        ESP_LOGW(TAG, "command %s: no endpoint exposes cluster 0x%04x", cmd.name, cluster_id);
        return ESP_ERR_NOT_FOUND;
    }

    en2m_event_post(EN2M_EVENT_COMMAND_RECEIVED, &event, sizeof(event));

    en2m_dm_lock();
    cluster = en2m_dm_find_cluster(endpoint_id, cluster_id);
    if (cluster != NULL) {
        cluster_cb = cluster->command_cb;
        cluster_ctx = cluster->command_ctx;
    }
    en2m_dm_unlock();

    if (cluster_cb != NULL) {
        err = cluster_cb(&cmd, cluster_ctx);
    }
    if (err == ESP_ERR_NOT_SUPPORTED && s_model.cfg.command != NULL) {
        err = s_model.cfg.command(&cmd, s_model.cfg.user_ctx);
    }
    if (err != ESP_ERR_NOT_SUPPORTED) {
        return err;
    }

    switch (id) {
    case EN2M_CMD_ON:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_ON_OFF, en2m_bool(true));
    case EN2M_CMD_OFF:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_ON_OFF, en2m_bool(false));
    case EN2M_CMD_TOGGLE: {
        en2m_value_t current;
        bool on = false;
        if (en2m_attribute_get(endpoint_id, cluster_id, EN2M_ATTR_ON_OFF, &current) == ESP_OK) {
            on = current.v.b;
        }
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_ON_OFF, en2m_bool(!on));
    }
    case EN2M_CMD_MOVE_TO_LEVEL: {
        int64_t level = en2m_value_as_int(&arg);
        uint8_t on_off_ep;
        if (level < 0) {
            level = 0;
        }
        if (level > 254) {
            level = 254;
        }
        err = en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_CURRENT_LEVEL, en2m_u8((uint8_t)level));
        on_off_ep = en2m_model_endpoint_with(EN2M_CLUSTER_ON_OFF);
        if (err == ESP_OK && level > 0 && on_off_ep != 0) {
            en2m_attribute_write(on_off_ep, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(true));
        }
        return err;
    }
    case EN2M_CMD_MOVE_TO_COLOR_TEMPERATURE:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_COLOR_TEMPERATURE_MIREDS,
                                    en2m_u16((uint16_t)en2m_value_as_int(&arg)));
    case EN2M_CMD_LOCK_DOOR:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_LOCK_STATE,
                                    en2m_enum8(EN2M_LOCK_LOCKED));
    case EN2M_CMD_UNLOCK_DOOR:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_LOCK_STATE,
                                    en2m_enum8(EN2M_LOCK_UNLOCKED));
    case EN2M_CMD_UP_OR_OPEN:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT,
                                    en2m_u8(0));
    case EN2M_CMD_DOWN_OR_CLOSE:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT,
                                    en2m_u8(100));
    case EN2M_CMD_GO_TO_LIFT_PERCENTAGE: {
        int64_t pos = en2m_value_as_int(&arg);
        if (pos < 0) {
            pos = 0;
        }
        if (pos > 100) {
            pos = 100;
        }
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT,
                                    en2m_u8((uint8_t)pos));
    }
    case EN2M_CMD_STOP_MOTION:
        /* Only the application knows where the motor actually halted. */
        ESP_LOGW(TAG, "stop needs a command handler or a window covering driver");
        return ESP_ERR_NOT_SUPPORTED;
    case EN2M_CMD_SET_FAN_MODE: {
        uint8_t mode = (uint8_t)en2m_value_as_int(&arg);
        err = en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_FAN_MODE, en2m_enum8(mode));
        if (err == ESP_OK && mode == EN2M_FAN_OFF) {
            en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_PERCENT_SETTING, en2m_u8(0));
        }
        return err;
    }
    case EN2M_CMD_SET_FAN_PERCENT: {
        int64_t pct = en2m_value_as_int(&arg);
        int64_t mode = 0;
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }
        err = en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_PERCENT_SETTING, en2m_u8((uint8_t)pct));
        if (err != ESP_OK) {
            return err;
        }
        en2m_model_read(endpoint_id, cluster_id, EN2M_ATTR_FAN_MODE, &mode);
        if (pct == 0 && mode != EN2M_FAN_OFF) {
            en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_FAN_MODE, en2m_enum8(EN2M_FAN_OFF));
        } else if (pct > 0 && mode == EN2M_FAN_OFF) {
            en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_FAN_MODE, en2m_enum8(EN2M_FAN_ON));
        }
        return ESP_OK;
    }
    case EN2M_CMD_SET_SYSTEM_MODE:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_SYSTEM_MODE,
                                    en2m_enum8((uint8_t)en2m_value_as_int(&arg)));
    case EN2M_CMD_SET_HEATING_SETPOINT:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_OCCUPIED_HEATING_SETPOINT,
                                    en2m_i16((int16_t)en2m_value_as_int(&arg)));
    case EN2M_CMD_SET_COOLING_SETPOINT:
        return en2m_attribute_write(endpoint_id, cluster_id, EN2M_ATTR_OCCUPIED_COOLING_SETPOINT,
                                    en2m_i16((int16_t)en2m_value_as_int(&arg)));
    case EN2M_CMD_IDENTIFY: {
        en2m_event_identify_t identify = {
            .endpoint_id = endpoint_id,
            .seconds = (uint16_t)en2m_value_as_int(&arg),
        };
        en2m_attribute_set(endpoint_id, EN2M_CLUSTER_IDENTIFY, EN2M_ATTR_IDENTIFY_TIME,
                           en2m_u16(identify.seconds));
        en2m_event_post(EN2M_EVENT_IDENTIFY, &identify, sizeof(identify));
        if (s_model.cfg.identify != NULL) {
            s_model.cfg.identify(endpoint_id, identify.seconds, s_model.cfg.user_ctx);
        }
        return ESP_OK;
    }
    default:
        ESP_LOGW(TAG, "unhandled command '%s' on cluster 0x%04x", cmd.name, cluster_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/** Dispatch on a cluster the request names by id, falling back to discovery. */
static esp_err_t en2m_model_exec_on_cluster(uint8_t endpoint_id, uint16_t cluster_id,
                                            en2m_command_id_t id, const char *name, en2m_value_t arg,
                                            uint16_t transaction_id, const char *json)
{
    if (endpoint_id == 0) {
        endpoint_id = en2m_model_endpoint_with(cluster_id);
    }
    return en2m_model_exec(endpoint_id, cluster_id, id, name, arg, transaction_id, json);
}

/**
 * Home Assistant / MQTT style payloads, e.g. {"switch":"ON"} or
 * {"brightness":128}. Returns the number of commands that were recognized.
 */
static int en2m_model_decode_flat(const cJSON *doc, uint8_t endpoint_id, uint16_t transaction_id,
                                  const char *json)
{
    const cJSON *item;
    int handled = 0;

    item = cJSON_GetObjectItem(doc, "switch");
    if (cJSON_IsString(item)) {
        en2m_command_id_t id = EN2M_CMD_OFF;
        if (strcmp(item->valuestring, "TOGGLE") == 0) {
            id = EN2M_CMD_TOGGLE;
        } else if (strcmp(item->valuestring, "ON") == 0) {
            id = EN2M_CMD_ON;
        }
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_ON_OFF, id, item->valuestring,
                                   (en2m_value_t){0}, transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "brightness");
    if (!cJSON_IsNumber(item)) {
        item = cJSON_GetObjectItem(doc, "level");
    }
    if (cJSON_IsNumber(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_LEVEL_CONTROL, EN2M_CMD_MOVE_TO_LEVEL,
                                   "brightness", en2m_u16((uint16_t)item->valueint), transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "color_temp");
    if (cJSON_IsNumber(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_COLOR_CONTROL,
                                   EN2M_CMD_MOVE_TO_COLOR_TEMPERATURE, "color_temp",
                                   en2m_u16((uint16_t)item->valueint), transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "position");
    if (cJSON_IsNumber(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_WINDOW_COVERING,
                                   EN2M_CMD_GO_TO_LIFT_PERCENTAGE, "position",
                                   en2m_u8((uint8_t)item->valueint), transaction_id, json);
        handled++;
    } else {
        item = cJSON_GetObjectItem(doc, "cover");
        if (cJSON_IsString(item)) {
            en2m_command_id_t id = EN2M_CMD_STOP_MOTION;
            if (strcmp(item->valuestring, "OPEN") == 0) {
                id = EN2M_CMD_UP_OR_OPEN;
            } else if (strcmp(item->valuestring, "CLOSE") == 0 || strcmp(item->valuestring, "CLOSED") == 0) {
                id = EN2M_CMD_DOWN_OR_CLOSE;
            }
            en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_WINDOW_COVERING, id, item->valuestring,
                                       (en2m_value_t){0}, transaction_id, json);
            handled++;
        }
    }

    item = cJSON_GetObjectItem(doc, "lock");
    if (cJSON_IsString(item)) {
        bool lock = (strcmp(item->valuestring, "LOCK") == 0 || strcmp(item->valuestring, "LOCKED") == 0);
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_DOOR_LOCK,
                                   lock ? EN2M_CMD_LOCK_DOOR : EN2M_CMD_UNLOCK_DOOR, item->valuestring,
                                   (en2m_value_t){0}, transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "fan_mode");
    if (cJSON_IsString(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_FAN_CONTROL, EN2M_CMD_SET_FAN_MODE,
                                   item->valuestring, en2m_enum8(en2m_fan_mode_parse(item->valuestring)),
                                   transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "percentage");
    if (cJSON_IsNumber(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_FAN_CONTROL, EN2M_CMD_SET_FAN_PERCENT,
                                   "percentage", en2m_u8((uint8_t)item->valueint), transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "hvac_mode");
    if (cJSON_IsString(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_THERMOSTAT, EN2M_CMD_SET_SYSTEM_MODE,
                                   item->valuestring, en2m_enum8(en2m_hvac_mode_parse(item->valuestring)),
                                   transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "target_temperature");
    if (cJSON_IsNumber(item)) {
        int64_t mode = EN2M_THERMOSTAT_HEAT;
        uint8_t ep = (endpoint_id != 0) ? endpoint_id : en2m_model_endpoint_with(EN2M_CLUSTER_THERMOSTAT);
        en2m_value_t centi = en2m_i16((int16_t)(item->valuedouble * 100.0));

        en2m_model_read(ep, EN2M_CLUSTER_THERMOSTAT, EN2M_ATTR_SYSTEM_MODE, &mode);
        en2m_model_exec_on_cluster(ep, EN2M_CLUSTER_THERMOSTAT,
                                   (mode == EN2M_THERMOSTAT_COOL) ? EN2M_CMD_SET_COOLING_SETPOINT
                                                                  : EN2M_CMD_SET_HEATING_SETPOINT,
                                   "target_temperature", centi, transaction_id, json);
        handled++;
    }

    item = cJSON_GetObjectItem(doc, "identify");
    if (cJSON_IsNumber(item)) {
        en2m_model_exec_on_cluster(endpoint_id, EN2M_CLUSTER_IDENTIFY, EN2M_CMD_IDENTIFY, "identify",
                                   en2m_u16((uint16_t)item->valueint), transaction_id, json);
        handled++;
    }

    return handled;
}

/** Cluster-addressed payloads, e.g. {"ep":1,"cluster":"on_off","command":"toggle"}. */
static const struct {
    const char *name;
    uint16_t id;
} k_cluster_names[] = {
    {"identify", EN2M_CLUSTER_IDENTIFY},
    {"on_off", EN2M_CLUSTER_ON_OFF},
    {"level_control", EN2M_CLUSTER_LEVEL_CONTROL},
    {"color_control", EN2M_CLUSTER_COLOR_CONTROL},
    {"door_lock", EN2M_CLUSTER_DOOR_LOCK},
    {"window_covering", EN2M_CLUSTER_WINDOW_COVERING},
    {"thermostat", EN2M_CLUSTER_THERMOSTAT},
    {"fan_control", EN2M_CLUSTER_FAN_CONTROL},
};

static bool en2m_cluster_id_from_name(const char *name, uint16_t *out)
{
    for (size_t i = 0; i < sizeof(k_cluster_names) / sizeof(k_cluster_names[0]); i++) {
        if (strcmp(k_cluster_names[i].name, name) == 0) {
            *out = k_cluster_names[i].id;
            return true;
        }
    }
    return false;
}

static en2m_command_id_t en2m_command_id_from_name(uint16_t cluster_id, const char *name)
{
    if (cluster_id == EN2M_CLUSTER_ON_OFF) {
        if (strcmp(name, "on") == 0) {
            return EN2M_CMD_ON;
        }
        if (strcmp(name, "off") == 0) {
            return EN2M_CMD_OFF;
        }
        if (strcmp(name, "toggle") == 0) {
            return EN2M_CMD_TOGGLE;
        }
    } else if (cluster_id == EN2M_CLUSTER_LEVEL_CONTROL) {
        if (strcmp(name, "move_to_level") == 0) {
            return EN2M_CMD_MOVE_TO_LEVEL;
        }
    } else if (cluster_id == EN2M_CLUSTER_COLOR_CONTROL) {
        if (strcmp(name, "move_to_color_temperature") == 0) {
            return EN2M_CMD_MOVE_TO_COLOR_TEMPERATURE;
        }
    } else if (cluster_id == EN2M_CLUSTER_DOOR_LOCK) {
        if (strcmp(name, "lock") == 0) {
            return EN2M_CMD_LOCK_DOOR;
        }
        if (strcmp(name, "unlock") == 0) {
            return EN2M_CMD_UNLOCK_DOOR;
        }
    } else if (cluster_id == EN2M_CLUSTER_WINDOW_COVERING) {
        if (strcmp(name, "open") == 0) {
            return EN2M_CMD_UP_OR_OPEN;
        }
        if (strcmp(name, "close") == 0) {
            return EN2M_CMD_DOWN_OR_CLOSE;
        }
        if (strcmp(name, "stop") == 0) {
            return EN2M_CMD_STOP_MOTION;
        }
        if (strcmp(name, "go_to") == 0) {
            return EN2M_CMD_GO_TO_LIFT_PERCENTAGE;
        }
    } else if (cluster_id == EN2M_CLUSTER_FAN_CONTROL) {
        if (strcmp(name, "set_mode") == 0) {
            return EN2M_CMD_SET_FAN_MODE;
        }
        if (strcmp(name, "set_percent") == 0) {
            return EN2M_CMD_SET_FAN_PERCENT;
        }
    } else if (cluster_id == EN2M_CLUSTER_THERMOSTAT) {
        if (strcmp(name, "set_mode") == 0) {
            return EN2M_CMD_SET_SYSTEM_MODE;
        }
        if (strcmp(name, "set_heating") == 0) {
            return EN2M_CMD_SET_HEATING_SETPOINT;
        }
        if (strcmp(name, "set_cooling") == 0) {
            return EN2M_CMD_SET_COOLING_SETPOINT;
        }
    } else if (cluster_id == EN2M_CLUSTER_IDENTIFY) {
        if (strcmp(name, "identify") == 0) {
            return EN2M_CMD_IDENTIFY;
        }
    }
    return EN2M_CMD_UNKNOWN;
}

static en2m_value_t en2m_command_arg(const cJSON *doc, uint16_t cluster_id, en2m_command_id_t id)
{
    const cJSON *item;

    switch (id) {
    case EN2M_CMD_MOVE_TO_LEVEL:
        item = cJSON_GetObjectItem(doc, "level");
        return cJSON_IsNumber(item) ? en2m_u16((uint16_t)item->valueint) : (en2m_value_t){0};
    case EN2M_CMD_MOVE_TO_COLOR_TEMPERATURE:
        item = cJSON_GetObjectItem(doc, "color_temp");
        return cJSON_IsNumber(item) ? en2m_u16((uint16_t)item->valueint) : (en2m_value_t){0};
    case EN2M_CMD_GO_TO_LIFT_PERCENTAGE:
        item = cJSON_GetObjectItem(doc, "position");
        return cJSON_IsNumber(item) ? en2m_u8((uint8_t)item->valueint) : (en2m_value_t){0};
    case EN2M_CMD_SET_FAN_MODE:
        item = cJSON_GetObjectItem(doc, "mode");
        return cJSON_IsString(item) ? en2m_enum8(en2m_fan_mode_parse(item->valuestring))
                                    : (en2m_value_t){0};
    case EN2M_CMD_SET_FAN_PERCENT:
        item = cJSON_GetObjectItem(doc, "percentage");
        return cJSON_IsNumber(item) ? en2m_u8((uint8_t)item->valueint) : (en2m_value_t){0};
    case EN2M_CMD_SET_SYSTEM_MODE:
        item = cJSON_GetObjectItem(doc, "mode");
        return cJSON_IsString(item) ? en2m_enum8(en2m_hvac_mode_parse(item->valuestring))
                                    : (en2m_value_t){0};
    case EN2M_CMD_SET_HEATING_SETPOINT:
    case EN2M_CMD_SET_COOLING_SETPOINT:
        item = cJSON_GetObjectItem(doc, "temperature");
        return cJSON_IsNumber(item) ? en2m_i16((int16_t)(item->valuedouble * 100.0)) : (en2m_value_t){0};
    case EN2M_CMD_IDENTIFY:
        item = cJSON_GetObjectItem(doc, "seconds");
        return cJSON_IsNumber(item) ? en2m_u16((uint16_t)item->valueint) : en2m_u16(10);
    default:
        (void)cluster_id;
        return (en2m_value_t){0};
    }
}

void en2m_model_on_command_frame(const en2m_pkt_t *pkt)
{
    char payload[EN2M_DATA_MAX + 1] = {0};
    const cJSON *ep_item;
    const cJSON *cluster_item;
    const cJSON *command_item;
    uint8_t endpoint_id = 0;
    uint16_t cluster_id = 0;
    cJSON *doc;

    if (!s_model.started) {
        return;
    }
    if (pkt->data_len > 0) {
        memcpy(payload, pkt->data, pkt->data_len);
    }

    doc = cJSON_Parse(payload);
    if (doc == NULL) {
        ESP_LOGW(TAG, "command payload is not valid JSON");
        return;
    }

    ep_item = cJSON_GetObjectItem(doc, "ep");
    if (cJSON_IsNumber(ep_item)) {
        endpoint_id = (uint8_t)ep_item->valueint;
    }

    cluster_item = cJSON_GetObjectItem(doc, "cluster");
    command_item = cJSON_GetObjectItem(doc, "command");
    if (cJSON_IsString(cluster_item) && cJSON_IsString(command_item) &&
        en2m_cluster_id_from_name(cluster_item->valuestring, &cluster_id)) {
        en2m_command_id_t id = en2m_command_id_from_name(cluster_id, command_item->valuestring);
        en2m_model_exec_on_cluster(endpoint_id, cluster_id, id, command_item->valuestring,
                                   en2m_command_arg(doc, cluster_id, id), pkt->cmd_id, payload);
    } else if (en2m_model_decode_flat(doc, endpoint_id, pkt->cmd_id, payload) == 0) {
        ESP_LOGW(TAG, "command payload had nothing this device understands");
    }

    cJSON_Delete(doc);

    if (pkt->cmd_id != 0) {
        en2m_send_uplink(EN2M_MSG_ACK, pkt->cmd_id, NULL, 0);
    }
    en2m_report_schedule(0);
}

/* ---- periodic work ---- */

/** Count the Identify cluster down once a second and re-trigger the effect. */
static void en2m_model_identify_tick(int64_t now_ms)
{
    if (now_ms < s_model.next_identify_ms) {
        return;
    }
    s_model.next_identify_ms = now_ms + 1000;

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        struct en2m_endpoint *ep = en2m_dm_endpoint_slot(i);
        int64_t remaining = 0;

        if (ep == NULL || !ep->used) {
            continue;
        }
        if (!en2m_model_read(ep->id, EN2M_CLUSTER_IDENTIFY, EN2M_ATTR_IDENTIFY_TIME, &remaining) ||
            remaining <= 0) {
            continue;
        }
        remaining--;
        en2m_attribute_set(ep->id, EN2M_CLUSTER_IDENTIFY, EN2M_ATTR_IDENTIFY_TIME,
                           en2m_u16((uint16_t)remaining));
        if (s_model.cfg.identify != NULL) {
            s_model.cfg.identify(ep->id, (uint16_t)remaining, s_model.cfg.user_ctx);
        }
    }
}

void en2m_model_tick(int64_t now_ms)
{
    bool periodic_due;

    if (!s_model.started) {
        return;
    }

    en2m_model_identify_tick(now_ms);

    if (now_ms - s_model.last_persist_ms >= EN2M_PERSIST_FLUSH_MS) {
        s_model.last_persist_ms = now_ms;
        en2m_dm_flush_persist();
    }

    periodic_due = (s_model.cfg.report_mode == EN2M_REPORT_DEFAULT ||
                    s_model.cfg.report_mode == EN2M_REPORT_PERIODIC_ONLY) &&
                   (now_ms - s_model.last_report_ms >= (int64_t)s_model.cfg.report_interval_ms);

    if (periodic_due || (s_model.next_report_ms != 0 && now_ms >= s_model.next_report_ms)) {
        en2m_dm_refresh();
        en2m_report_transmit();
    }
}

/* ---- lifecycle ---- */

/** Push restored values back into the hardware so it matches the reported state. */
static void en2m_model_apply_persisted(void)
{
    for (int e = 0; e < EN2M_MAX_ENDPOINTS; e++) {
        struct en2m_endpoint *ep = en2m_dm_endpoint_slot(e);
        if (ep == NULL || !ep->used) {
            continue;
        }
        for (int c = 0; c < EN2M_MAX_CLUSTERS_PER_ENDPOINT; c++) {
            for (int a = 0; a < EN2M_MAX_ATTRIBUTES_PER_CLUSTER; a++) {
                en2m_attr_path_t path;
                en2m_value_t value;

                en2m_dm_lock();
                if (!ep->clusters[c].used || !ep->clusters[c].attrs[a].used ||
                    !ep->clusters[c].attrs[a].persist) {
                    en2m_dm_unlock();
                    continue;
                }
                path.endpoint_id = ep->id;
                path.cluster_id = ep->clusters[c].id;
                path.attribute_id = ep->clusters[c].attrs[a].id;
                value = ep->clusters[c].attrs[a].value;
                en2m_dm_unlock();

                en2m_attribute_write(path.endpoint_id, path.cluster_id, path.attribute_id, value);
            }
        }
    }
}

esp_err_t en2m_start(const en2m_device_config_t *config)
{
    en2m_device_config_t cfg;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_model.started, ESP_ERR_INVALID_STATE, TAG, "already started");

    cfg = *config;
    if (cfg.report_interval_ms == 0) {
        cfg.report_interval_ms = (cfg.mesh.role == EN2M_ROLE_LEAF) ? EN2M_REPORT_INTERVAL_LEAF_MS
                                                                   : EN2M_REPORT_INTERVAL_MAINS_MS;
    }
    if (cfg.min_report_interval_ms == 0) {
        cfg.min_report_interval_ms = EN2M_MIN_REPORT_INTERVAL_MS;
    }
    s_model.cfg = cfg;
    s_model.configured = true;

    ESP_RETURN_ON_ERROR(en2m_mesh_init(&s_model.cfg.mesh), TAG, "mesh init failed");

    en2m_dm_restore();
    en2m_model_apply_persisted();

    s_model.last_report_ms = en2m_now_ms();
    s_model.last_persist_ms = s_model.last_report_ms;
    s_model.started = true;
    s_model.next_report_ms = s_model.last_report_ms + 500;

    en2m_event_post(EN2M_EVENT_STARTED, NULL, 0);
    return ESP_OK;
}

esp_err_t en2m_stop(void)
{
    if (!s_model.started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_model.started = false;
    en2m_dm_flush_persist();
    en2m_mesh_deinit();
    en2m_event_post(EN2M_EVENT_STOPPED, NULL, 0);
    return ESP_OK;
}

/* ---- deprecated shims ---- */

esp_err_t en2m_model_start(const en2m_config_t *mesh_config)
{
    en2m_device_config_t cfg = {0};

    ESP_RETURN_ON_FALSE(mesh_config != NULL, ESP_ERR_INVALID_ARG, TAG, "mesh_config is NULL");
    cfg.mesh = *mesh_config;
    return en2m_start(&cfg);
}

void en2m_model_loop(void)
{
    static bool warned;

    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "en2m_model_loop() is obsolete: the component runs its own task");
    }
}

esp_err_t en2m_model_report(void)
{
    return en2m_report_now();
}

esp_err_t en2m_model_notify(uint8_t endpoint_id, uint16_t cluster_id, bool immediate)
{
    (void)endpoint_id;
    (void)cluster_id;
    return immediate ? en2m_report_now() : en2m_report_schedule(0);
}
