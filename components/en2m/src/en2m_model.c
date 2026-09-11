/**
 * @file en2m_model.c
 * @brief Endpoint / cluster interaction layer (no hardware drivers).
 *
 * Uplink JSON stays compact (EN2M_DATA_MAX): flat HA aliases + caps +
 * a short cluster name list. Nested attribute dumps are omitted on-air.
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "en2m_model.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "en2m_model";

typedef struct {
    bool present;
    en2m_on_off_driver_t on_off;
    en2m_level_driver_t level;
    en2m_color_control_driver_t color;
    en2m_boolean_state_driver_t boolean_state;
    en2m_occupancy_driver_t occupancy;
    en2m_illuminance_driver_t illuminance;
    en2m_temperature_driver_t temperature;
    en2m_humidity_driver_t humidity;
    en2m_pressure_driver_t pressure;
    en2m_electrical_power_driver_t electrical;
    en2m_fan_control_driver_t fan;
    en2m_window_covering_driver_t cover;
    en2m_door_lock_driver_t lock;
    en2m_thermostat_driver_t thermostat;
    en2m_smoke_co_driver_t smoke_co;
} en2m_endpoint_clusters_t;

struct en2m_endpoint {
    bool used;
    uint8_t id;
    en2m_endpoint_clusters_t clusters;
};

static struct {
    bool started;
    en2m_endpoint_t endpoints[EN2M_MAX_ENDPOINTS];
    int64_t last_report_us;
    uint32_t report_interval_ms;
} s_model;

static int64_t en2m_model_now_us(void)
{
    return esp_timer_get_time();
}

static en2m_endpoint_t *en2m_endpoint_find(uint8_t endpoint_id)
{
    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (s_model.endpoints[i].used && s_model.endpoints[i].id == endpoint_id) {
            return &s_model.endpoints[i];
        }
    }
    return NULL;
}

en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id)
{
    if (endpoint_id == 0 || endpoint_id == 255) {
        ESP_LOGE(TAG, "invalid endpoint id %u", endpoint_id);
        return NULL;
    }
    if (en2m_endpoint_find(endpoint_id) != NULL) {
        ESP_LOGE(TAG, "endpoint %u already exists", endpoint_id);
        return NULL;
    }

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (!s_model.endpoints[i].used) {
            memset(&s_model.endpoints[i], 0, sizeof(s_model.endpoints[i]));
            s_model.endpoints[i].used = true;
            s_model.endpoints[i].id = endpoint_id;
            return &s_model.endpoints[i];
        }
    }

    ESP_LOGE(TAG, "no free endpoint slot");
    return NULL;
}

#define EN2M_ADD_DRIVER(field, required_fn)                                                                    \
    do {                                                                                                       \
        ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL && (required_fn), ESP_ERR_INVALID_ARG, TAG, "bad args"); \
        ep->clusters.present = true;                                                                           \
        ep->clusters.field = *driver;                                                                          \
        return ESP_OK;                                                                                         \
    } while (0)

esp_err_t en2m_endpoint_add_on_off(en2m_endpoint_t *ep, const en2m_on_off_driver_t *driver)
{
    EN2M_ADD_DRIVER(on_off, driver->get != NULL);
}

esp_err_t en2m_endpoint_add_level_control(en2m_endpoint_t *ep, const en2m_level_driver_t *driver)
{
    EN2M_ADD_DRIVER(level, driver->get_level != NULL);
}

esp_err_t en2m_endpoint_add_color_control(en2m_endpoint_t *ep, const en2m_color_control_driver_t *driver)
{
    EN2M_ADD_DRIVER(color, driver->get_color_temp != NULL);
}

esp_err_t en2m_endpoint_add_boolean_state(en2m_endpoint_t *ep, const en2m_boolean_state_driver_t *driver)
{
    EN2M_ADD_DRIVER(boolean_state, driver->get != NULL);
}

esp_err_t en2m_endpoint_add_occupancy(en2m_endpoint_t *ep, const en2m_occupancy_driver_t *driver)
{
    EN2M_ADD_DRIVER(occupancy, driver->get_occupied != NULL);
}

esp_err_t en2m_endpoint_add_illuminance(en2m_endpoint_t *ep, const en2m_illuminance_driver_t *driver)
{
    EN2M_ADD_DRIVER(illuminance, driver->get_lux != NULL);
}

esp_err_t en2m_endpoint_add_temperature(en2m_endpoint_t *ep, const en2m_temperature_driver_t *driver)
{
    EN2M_ADD_DRIVER(temperature, driver->get_measured_value != NULL);
}

esp_err_t en2m_endpoint_add_humidity(en2m_endpoint_t *ep, const en2m_humidity_driver_t *driver)
{
    EN2M_ADD_DRIVER(humidity, driver->get_measured_value != NULL);
}

esp_err_t en2m_endpoint_add_pressure(en2m_endpoint_t *ep, const en2m_pressure_driver_t *driver)
{
    EN2M_ADD_DRIVER(pressure, driver->get_hpa != NULL);
}

esp_err_t en2m_endpoint_add_electrical_power(en2m_endpoint_t *ep, const en2m_electrical_power_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ep->clusters.present = true;
    ep->clusters.electrical = *driver;
    return ESP_OK;
}

esp_err_t en2m_endpoint_add_fan_control(en2m_endpoint_t *ep, const en2m_fan_control_driver_t *driver)
{
    EN2M_ADD_DRIVER(fan, driver->get_mode != NULL);
}

esp_err_t en2m_endpoint_add_window_covering(en2m_endpoint_t *ep, const en2m_window_covering_driver_t *driver)
{
    EN2M_ADD_DRIVER(cover, driver->get_position != NULL);
}

esp_err_t en2m_endpoint_add_door_lock(en2m_endpoint_t *ep, const en2m_door_lock_driver_t *driver)
{
    EN2M_ADD_DRIVER(lock, driver->get_locked != NULL);
}

esp_err_t en2m_endpoint_add_thermostat(en2m_endpoint_t *ep, const en2m_thermostat_driver_t *driver)
{
    EN2M_ADD_DRIVER(thermostat, driver->get_system_mode != NULL);
}

esp_err_t en2m_endpoint_add_smoke_co(en2m_endpoint_t *ep, const en2m_smoke_co_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ep->clusters.present = true;
    ep->clusters.smoke_co = *driver;
    return ESP_OK;
}

static void en2m_model_append_caps(cJSON *caps, const char *name)
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

static const char *en2m_fan_mode_str(en2m_fan_mode_t mode)
{
    switch (mode) {
    case EN2M_FAN_OFF:
        return "off";
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
    default:
        return "off";
    }
}

static en2m_fan_mode_t en2m_fan_mode_parse(const char *s)
{
    if (!s) {
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

static const char *en2m_hvac_mode_str(en2m_thermostat_mode_t mode)
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
    if (!s) {
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

static cJSON *en2m_model_build_report_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    cJSON *clist = cJSON_AddArrayToObject(root, "clusters");
    const char *role = "leaf";
    bool has_level = false;
    bool has_color = false;
    bool has_on_off = false;

    if (en2m_get_role() == EN2M_ROLE_ROUTER) {
        role = "router";
    } else if (en2m_get_role() == EN2M_ROLE_COORDINATOR) {
        role = "coordinator";
    }
    cJSON_AddStringToObject(root, "node_role", role);

    if (en2m_has_parent()) {
        uint8_t mac[6];
        char mac_str[18];
        en2m_get_parent_mac(mac);
        en2m_mac_to_str(mac, mac_str);
        cJSON_AddNumberToObject(root, "path_cost", en2m_get_path_cost());
        cJSON_AddStringToObject(root, "parent", mac_str);
    }

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        en2m_endpoint_t *ep = &s_model.endpoints[i];
        if (!ep->used) {
            continue;
        }

        if (ep->clusters.on_off.get != NULL) {
            bool on = false;
            has_on_off = true;
            cJSON_AddItemToArray(clist, cJSON_CreateString("on_off"));
            if (ep->clusters.on_off.get(&on, ep->clusters.on_off.ctx) == ESP_OK) {
                cJSON_AddStringToObject(root, "switch", on ? "ON" : "OFF");
            }
        }

        if (ep->clusters.level.get_level != NULL) {
            uint8_t level = 0;
            has_level = true;
            cJSON_AddItemToArray(clist, cJSON_CreateString("level"));
            if (ep->clusters.level.get_level(&level, ep->clusters.level.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "brightness", level);
                cJSON_AddNumberToObject(root, "level", level);
            }
        }

        if (ep->clusters.color.get_color_temp != NULL) {
            uint16_t mireds = 0;
            has_color = true;
            cJSON_AddItemToArray(clist, cJSON_CreateString("color"));
            if (ep->clusters.color.get_color_temp(&mireds, ep->clusters.color.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "color_temp", mireds);
                cJSON_AddStringToObject(root, "color_mode", "color_temp");
            }
        }

        if (ep->clusters.boolean_state.get != NULL) {
            bool value = false;
            cJSON_AddItemToArray(clist, cJSON_CreateString("boolean"));
            if (ep->clusters.boolean_state.get(&value, ep->clusters.boolean_state.ctx) == ESP_OK) {
                cJSON_AddStringToObject(root, "contact", value ? "ON" : "OFF");
            }
            en2m_model_append_caps(caps, "contact");
        }

        if (ep->clusters.occupancy.get_occupied != NULL) {
            bool occ = false;
            cJSON_AddItemToArray(clist, cJSON_CreateString("occupancy"));
            if (ep->clusters.occupancy.get_occupied(&occ, ep->clusters.occupancy.ctx) == ESP_OK) {
                cJSON_AddStringToObject(root, "occupancy", occ ? "ON" : "OFF");
                cJSON_AddStringToObject(root, "motion", occ ? "ON" : "OFF");
            }
            en2m_model_append_caps(caps, "occupancy");
            en2m_model_append_caps(caps, "motion");
        }

        if (ep->clusters.illuminance.get_lux != NULL) {
            uint32_t lux = 0;
            cJSON_AddItemToArray(clist, cJSON_CreateString("illuminance"));
            if (ep->clusters.illuminance.get_lux(&lux, ep->clusters.illuminance.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "illuminance", (double)lux);
            }
            en2m_model_append_caps(caps, "illuminance");
        }

        if (ep->clusters.temperature.get_measured_value != NULL) {
            int16_t centi = 0;
            cJSON_AddItemToArray(clist, cJSON_CreateString("temp"));
            if (ep->clusters.temperature.get_measured_value(&centi, ep->clusters.temperature.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "temperature", centi / 100.0);
            }
            en2m_model_append_caps(caps, "temperature");
        }

        if (ep->clusters.humidity.get_measured_value != NULL) {
            uint16_t centi = 0;
            cJSON_AddItemToArray(clist, cJSON_CreateString("humidity"));
            if (ep->clusters.humidity.get_measured_value(&centi, ep->clusters.humidity.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "humidity", centi / 100.0);
            }
            en2m_model_append_caps(caps, "humidity");
        }

        if (ep->clusters.pressure.get_hpa != NULL) {
            int32_t hpa_x10 = 0;
            cJSON_AddItemToArray(clist, cJSON_CreateString("pressure"));
            if (ep->clusters.pressure.get_hpa(&hpa_x10, ep->clusters.pressure.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "pressure", hpa_x10 / 10.0);
            }
            en2m_model_append_caps(caps, "pressure");
        }

        if (ep->clusters.electrical.get_active_power != NULL || ep->clusters.electrical.get_energy != NULL) {
            cJSON_AddItemToArray(clist, cJSON_CreateString("electrical"));
            if (ep->clusters.electrical.get_active_power != NULL) {
                int32_t mw = 0;
                if (ep->clusters.electrical.get_active_power(&mw, ep->clusters.electrical.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "power", mw / 1000.0);
                    en2m_model_append_caps(caps, "power");
                }
            }
            if (ep->clusters.electrical.get_energy != NULL) {
                int64_t mwh = 0;
                if (ep->clusters.electrical.get_energy(&mwh, ep->clusters.electrical.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "energy", mwh / 1000.0);
                    en2m_model_append_caps(caps, "energy");
                }
            }
        }

        if (ep->clusters.fan.get_mode != NULL) {
            en2m_fan_mode_t mode = EN2M_FAN_OFF;
            cJSON_AddItemToArray(clist, cJSON_CreateString("fan"));
            if (ep->clusters.fan.get_mode(&mode, ep->clusters.fan.ctx) == ESP_OK) {
                cJSON_AddStringToObject(root, "fan_mode", en2m_fan_mode_str(mode));
            }
            if (ep->clusters.fan.get_percent != NULL) {
                uint8_t pct = 0;
                if (ep->clusters.fan.get_percent(&pct, ep->clusters.fan.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "percentage", pct);
                }
            }
            en2m_model_append_caps(caps, "fan");
        }

        if (ep->clusters.cover.get_position != NULL) {
            uint8_t pos = 0;
            cJSON_AddItemToArray(clist, cJSON_CreateString("cover"));
            if (ep->clusters.cover.get_position(&pos, ep->clusters.cover.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(root, "position", pos);
                /* HA cover: 0 open, 100 closed → current_cover inverted for friendliness */
                cJSON_AddStringToObject(root, "cover", pos >= 95 ? "CLOSED" : (pos <= 5 ? "OPEN" : "OPEN"));
            }
            en2m_model_append_caps(caps, "cover");
        }

        if (ep->clusters.lock.get_locked != NULL) {
            bool locked = false;
            cJSON_AddItemToArray(clist, cJSON_CreateString("lock"));
            if (ep->clusters.lock.get_locked(&locked, ep->clusters.lock.ctx) == ESP_OK) {
                cJSON_AddStringToObject(root, "lock", locked ? "LOCKED" : "UNLOCKED");
            }
            en2m_model_append_caps(caps, "lock");
        }

        if (ep->clusters.thermostat.get_system_mode != NULL) {
            en2m_thermostat_mode_t mode = EN2M_THERMOSTAT_OFF;
            cJSON_AddItemToArray(clist, cJSON_CreateString("thermostat"));
            if (ep->clusters.thermostat.get_system_mode(&mode, ep->clusters.thermostat.ctx) == ESP_OK) {
                cJSON_AddStringToObject(root, "hvac_mode", en2m_hvac_mode_str(mode));
            }
            if (ep->clusters.thermostat.get_local_temperature != NULL) {
                int16_t t = 0;
                if (ep->clusters.thermostat.get_local_temperature(&t, ep->clusters.thermostat.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "current_temperature", t / 100.0);
                }
            }
            if (ep->clusters.thermostat.get_occupied_heating != NULL) {
                int16_t t = 0;
                if (ep->clusters.thermostat.get_occupied_heating(&t, ep->clusters.thermostat.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "target_temperature", t / 100.0);
                    cJSON_AddNumberToObject(root, "target_temp_high", t / 100.0);
                }
            }
            if (ep->clusters.thermostat.get_occupied_cooling != NULL) {
                int16_t t = 0;
                if (ep->clusters.thermostat.get_occupied_cooling(&t, ep->clusters.thermostat.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "target_temp_low", t / 100.0);
                    if (cJSON_GetObjectItem(root, "target_temperature") == NULL) {
                        cJSON_AddNumberToObject(root, "target_temperature", t / 100.0);
                    }
                }
            }
            en2m_model_append_caps(caps, "climate");
        }

        if (ep->clusters.smoke_co.get_smoke != NULL || ep->clusters.smoke_co.get_co != NULL) {
            cJSON_AddItemToArray(clist, cJSON_CreateString("smoke_co"));
            if (ep->clusters.smoke_co.get_smoke != NULL) {
                bool alarm = false;
                if (ep->clusters.smoke_co.get_smoke(&alarm, ep->clusters.smoke_co.ctx) == ESP_OK) {
                    cJSON_AddStringToObject(root, "smoke", alarm ? "ON" : "OFF");
                    en2m_model_append_caps(caps, "smoke");
                }
            }
            if (ep->clusters.smoke_co.get_co != NULL) {
                bool alarm = false;
                if (ep->clusters.smoke_co.get_co(&alarm, ep->clusters.smoke_co.ctx) == ESP_OK) {
                    cJSON_AddStringToObject(root, "carbon_monoxide", alarm ? "ON" : "OFF");
                    en2m_model_append_caps(caps, "carbon_monoxide");
                }
            }
        }
    }

    /* Product-style caps for HA platforms */
    if (has_level || has_color) {
        en2m_model_append_caps(caps, "light");
    } else if (has_on_off) {
        en2m_model_append_caps(caps, "switch");
    }

    return root;
}

esp_err_t en2m_model_report(void)
{
    cJSON *root;
    char *printed;
    esp_err_t err;
    size_t len;

    ESP_RETURN_ON_FALSE(s_model.started, ESP_ERR_INVALID_STATE, TAG, "model not started");

    root = en2m_model_build_report_json();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "json alloc failed");

    printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed != NULL, ESP_ERR_NO_MEM, TAG, "json print failed");

    len = strnlen(printed, EN2M_DATA_MAX + 1);
    if (len > EN2M_DATA_MAX) {
        ESP_LOGW(TAG, "report truncated %u > %u", (unsigned)len, EN2M_DATA_MAX);
        len = EN2M_DATA_MAX;
    }

    err = en2m_send_uplink(EN2M_MSG_STATE, 0, (const uint8_t *)printed, (uint8_t)len);
    cJSON_free(printed);
    s_model.last_report_us = en2m_model_now_us();
    return err;
}

static en2m_endpoint_t *en2m_model_first_with_on_off(void)
{
    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (s_model.endpoints[i].used && s_model.endpoints[i].clusters.on_off.set != NULL) {
            return &s_model.endpoints[i];
        }
    }
    return NULL;
}

static bool en2m_model_apply_flat_command(cJSON *doc)
{
    bool acted = false;
    const cJSON *sw = cJSON_GetObjectItem(doc, "switch");
    const cJSON *bri = cJSON_GetObjectItem(doc, "brightness");
    const cJSON *level = cJSON_GetObjectItem(doc, "level");
    const cJSON *ct = cJSON_GetObjectItem(doc, "color_temp");
    const cJSON *cover = cJSON_GetObjectItem(doc, "cover");
    const cJSON *pos = cJSON_GetObjectItem(doc, "position");
    const cJSON *lock = cJSON_GetObjectItem(doc, "lock");
    const cJSON *fan_mode = cJSON_GetObjectItem(doc, "fan_mode");
    const cJSON *pct = cJSON_GetObjectItem(doc, "percentage");
    const cJSON *hvac = cJSON_GetObjectItem(doc, "hvac_mode");
    const cJSON *target = cJSON_GetObjectItem(doc, "target_temperature");

    if (cJSON_IsString(sw)) {
        en2m_endpoint_t *ep = en2m_model_first_with_on_off();
        if (ep != NULL) {
            bool on = (strcmp(sw->valuestring, "ON") == 0);
            if (strcmp(sw->valuestring, "TOGGLE") == 0) {
                bool cur = false;
                if (ep->clusters.on_off.get) {
                    ep->clusters.on_off.get(&cur, ep->clusters.on_off.ctx);
                }
                on = !cur;
            }
            ep->clusters.on_off.set(on, ep->clusters.on_off.ctx);
            acted = true;
        }
    }

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        en2m_endpoint_t *ep = &s_model.endpoints[i];
        if (!ep->used) {
            continue;
        }

        if ((cJSON_IsNumber(bri) || cJSON_IsNumber(level)) && ep->clusters.level.set_level != NULL) {
            int v = cJSON_IsNumber(bri) ? bri->valueint : level->valueint;
            if (v < 0) {
                v = 0;
            }
            if (v > 254) {
                v = 254;
            }
            ep->clusters.level.set_level((uint8_t)v, ep->clusters.level.ctx);
            if (ep->clusters.on_off.set != NULL && v > 0) {
                ep->clusters.on_off.set(true, ep->clusters.on_off.ctx);
            }
            acted = true;
        }

        if (cJSON_IsNumber(ct) && ep->clusters.color.set_color_temp != NULL) {
            ep->clusters.color.set_color_temp((uint16_t)ct->valueint, ep->clusters.color.ctx);
            acted = true;
        }

        if (ep->clusters.cover.command != NULL && (cJSON_IsString(cover) || cJSON_IsNumber(pos))) {
            if (cJSON_IsNumber(pos)) {
                ep->clusters.cover.command(EN2M_COVER_GOTO, (uint8_t)pos->valueint, ep->clusters.cover.ctx);
            } else if (strcmp(cover->valuestring, "OPEN") == 0) {
                ep->clusters.cover.command(EN2M_COVER_OPEN, 0, ep->clusters.cover.ctx);
            } else if (strcmp(cover->valuestring, "CLOSE") == 0) {
                ep->clusters.cover.command(EN2M_COVER_CLOSE, 100, ep->clusters.cover.ctx);
            } else if (strcmp(cover->valuestring, "STOP") == 0) {
                ep->clusters.cover.command(EN2M_COVER_STOP, 0, ep->clusters.cover.ctx);
            }
            acted = true;
        }

        if (cJSON_IsString(lock) && ep->clusters.lock.lock != NULL) {
            if (strcmp(lock->valuestring, "LOCK") == 0 || strcmp(lock->valuestring, "LOCKED") == 0) {
                ep->clusters.lock.lock(ep->clusters.lock.ctx);
            } else {
                ep->clusters.lock.unlock(ep->clusters.lock.ctx);
            }
            acted = true;
        }

        if (cJSON_IsString(fan_mode) && ep->clusters.fan.set_mode != NULL) {
            ep->clusters.fan.set_mode(en2m_fan_mode_parse(fan_mode->valuestring), ep->clusters.fan.ctx);
            acted = true;
        }
        if (cJSON_IsNumber(pct) && ep->clusters.fan.set_percent != NULL) {
            uint8_t p = (uint8_t)pct->valueint;
            if (p > 100) {
                p = 100;
            }
            ep->clusters.fan.set_percent(p, ep->clusters.fan.ctx);
            acted = true;
        }

        if (cJSON_IsString(hvac) && ep->clusters.thermostat.set_system_mode != NULL) {
            ep->clusters.thermostat.set_system_mode(en2m_hvac_mode_parse(hvac->valuestring),
                                                    ep->clusters.thermostat.ctx);
            acted = true;
        }
        if (cJSON_IsNumber(target) && ep->clusters.thermostat.set_occupied_heating != NULL) {
            int16_t centi = (int16_t)(target->valuedouble * 100.0);
            ep->clusters.thermostat.set_occupied_heating(centi, ep->clusters.thermostat.ctx);
            if (ep->clusters.thermostat.set_occupied_cooling != NULL) {
                ep->clusters.thermostat.set_occupied_cooling(centi, ep->clusters.thermostat.ctx);
            }
            acted = true;
        }
    }

    return acted;
}

static esp_err_t en2m_model_handle_command(const en2m_pkt_t *pkt, void *user_ctx)
{
    char tmp[EN2M_DATA_MAX + 1] = {0};
    cJSON *doc;
    const cJSON *ep_j;
    const cJSON *cluster_j;
    const cJSON *cmd_j;
    en2m_endpoint_t *ep;
    uint8_t ep_id = 1;

    (void)user_ctx;
    if (pkt->data_len > 0) {
        memcpy(tmp, pkt->data, pkt->data_len);
    }

    doc = cJSON_Parse(tmp);
    if (doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Flat HA / MQTT style commands first */
    if (en2m_model_apply_flat_command(doc)) {
        cJSON_Delete(doc);
        en2m_model_report();
        return ESP_OK;
    }

    ep_j = cJSON_GetObjectItem(doc, "ep");
    if (cJSON_IsNumber(ep_j)) {
        ep_id = (uint8_t)ep_j->valueint;
    }

    ep = en2m_endpoint_find(ep_id);
    if (ep == NULL) {
        cJSON_Delete(doc);
        return ESP_ERR_NOT_FOUND;
    }

    cluster_j = cJSON_GetObjectItem(doc, "cluster");
    cmd_j = cJSON_GetObjectItem(doc, "command");
    if (!cJSON_IsString(cluster_j) || !cJSON_IsString(cmd_j)) {
        cJSON_Delete(doc);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(cluster_j->valuestring, "on_off") == 0 && ep->clusters.on_off.set != NULL) {
        bool on = false;
        bool cur = false;
        if (strcmp(cmd_j->valuestring, "on") == 0) {
            on = true;
        } else if (strcmp(cmd_j->valuestring, "off") == 0) {
            on = false;
        } else if (strcmp(cmd_j->valuestring, "toggle") == 0) {
            if (ep->clusters.on_off.get) {
                ep->clusters.on_off.get(&cur, ep->clusters.on_off.ctx);
            }
            on = !cur;
        }
        ep->clusters.on_off.set(on, ep->clusters.on_off.ctx);
    } else if (strcmp(cluster_j->valuestring, "level_control") == 0 && ep->clusters.level.set_level != NULL) {
        const cJSON *level_j = cJSON_GetObjectItem(doc, "level");
        if (cJSON_IsNumber(level_j)) {
            ep->clusters.level.set_level((uint8_t)level_j->valueint, ep->clusters.level.ctx);
        }
    } else if (strcmp(cluster_j->valuestring, "color_control") == 0 && ep->clusters.color.set_color_temp != NULL) {
        const cJSON *ct_j = cJSON_GetObjectItem(doc, "color_temp");
        if (cJSON_IsNumber(ct_j)) {
            ep->clusters.color.set_color_temp((uint16_t)ct_j->valueint, ep->clusters.color.ctx);
        }
    } else if (strcmp(cluster_j->valuestring, "window_covering") == 0 && ep->clusters.cover.command != NULL) {
        const cJSON *pos_j = cJSON_GetObjectItem(doc, "position");
        uint8_t p = cJSON_IsNumber(pos_j) ? (uint8_t)pos_j->valueint : 0;
        if (strcmp(cmd_j->valuestring, "open") == 0) {
            ep->clusters.cover.command(EN2M_COVER_OPEN, 0, ep->clusters.cover.ctx);
        } else if (strcmp(cmd_j->valuestring, "close") == 0) {
            ep->clusters.cover.command(EN2M_COVER_CLOSE, 100, ep->clusters.cover.ctx);
        } else if (strcmp(cmd_j->valuestring, "stop") == 0) {
            ep->clusters.cover.command(EN2M_COVER_STOP, 0, ep->clusters.cover.ctx);
        } else if (strcmp(cmd_j->valuestring, "go_to") == 0) {
            ep->clusters.cover.command(EN2M_COVER_GOTO, p, ep->clusters.cover.ctx);
        }
    } else if (strcmp(cluster_j->valuestring, "door_lock") == 0) {
        if (strcmp(cmd_j->valuestring, "lock") == 0 && ep->clusters.lock.lock) {
            ep->clusters.lock.lock(ep->clusters.lock.ctx);
        } else if (strcmp(cmd_j->valuestring, "unlock") == 0 && ep->clusters.lock.unlock) {
            ep->clusters.lock.unlock(ep->clusters.lock.ctx);
        }
    } else if (strcmp(cluster_j->valuestring, "fan_control") == 0) {
        if (strcmp(cmd_j->valuestring, "set_mode") == 0 && ep->clusters.fan.set_mode) {
            const cJSON *m = cJSON_GetObjectItem(doc, "mode");
            if (cJSON_IsString(m)) {
                ep->clusters.fan.set_mode(en2m_fan_mode_parse(m->valuestring), ep->clusters.fan.ctx);
            }
        } else if (strcmp(cmd_j->valuestring, "set_percent") == 0 && ep->clusters.fan.set_percent) {
            const cJSON *p = cJSON_GetObjectItem(doc, "percentage");
            if (cJSON_IsNumber(p)) {
                ep->clusters.fan.set_percent((uint8_t)p->valueint, ep->clusters.fan.ctx);
            }
        }
    } else if (strcmp(cluster_j->valuestring, "thermostat") == 0) {
        if (strcmp(cmd_j->valuestring, "set_mode") == 0 && ep->clusters.thermostat.set_system_mode) {
            const cJSON *m = cJSON_GetObjectItem(doc, "mode");
            if (cJSON_IsString(m)) {
                ep->clusters.thermostat.set_system_mode(en2m_hvac_mode_parse(m->valuestring),
                                                        ep->clusters.thermostat.ctx);
            }
        } else if (strcmp(cmd_j->valuestring, "set_heating") == 0 &&
                   ep->clusters.thermostat.set_occupied_heating) {
            const cJSON *t = cJSON_GetObjectItem(doc, "temperature");
            if (cJSON_IsNumber(t)) {
                ep->clusters.thermostat.set_occupied_heating((int16_t)(t->valuedouble * 100.0),
                                                             ep->clusters.thermostat.ctx);
            }
        } else if (strcmp(cmd_j->valuestring, "set_cooling") == 0 &&
                   ep->clusters.thermostat.set_occupied_cooling) {
            const cJSON *t = cJSON_GetObjectItem(doc, "temperature");
            if (cJSON_IsNumber(t)) {
                ep->clusters.thermostat.set_occupied_cooling((int16_t)(t->valuedouble * 100.0),
                                                             ep->clusters.thermostat.ctx);
            }
        }
    }

    cJSON_Delete(doc);
    en2m_model_report();
    return ESP_OK;
}

static void en2m_model_on_command(const en2m_pkt_t *pkt, void *user_ctx)
{
    en2m_model_handle_command(pkt, user_ctx);
}

esp_err_t en2m_model_start(const en2m_config_t *mesh_config)
{
    en2m_config_t cfg;

    ESP_RETURN_ON_FALSE(mesh_config != NULL, ESP_ERR_INVALID_ARG, TAG, "mesh_config NULL");

    cfg = *mesh_config;
    if (cfg.on_command == NULL) {
        cfg.on_command = en2m_model_on_command;
    }

    ESP_RETURN_ON_ERROR(en2m_mesh_init(&cfg), TAG, "mesh init failed");
    s_model.started = true;
    s_model.report_interval_ms = (cfg.role == EN2M_ROLE_LEAF) ? 30000 : 15000;
    en2m_model_report();
    return ESP_OK;
}

void en2m_model_loop(void)
{
    en2m_mesh_loop();
    if (!s_model.started) {
        return;
    }
    if ((en2m_model_now_us() - s_model.last_report_us) / 1000 >= s_model.report_interval_ms) {
        en2m_model_report();
    }
}

esp_err_t en2m_model_notify(uint8_t endpoint_id, en2m_cluster_id_t cluster, bool immediate)
{
    (void)endpoint_id;
    (void)cluster;
    if (immediate) {
        return en2m_model_report();
    }
    s_model.last_report_us = 0; /* force soon */
    return ESP_OK;
}
