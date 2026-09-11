/**
 * @file en2m_datamodel.c
 * @brief The attribute store: endpoints, clusters, attributes and their access paths.
 *
 * Two access paths exist on purpose and they are not interchangeable:
 *
 *  - ::en2m_attribute_set is the sensor path. The application already knows
 *    the truth, so the value is committed and reported.
 *  - ::en2m_attribute_write is the actuator path. The write callback runs
 *    first and the value is committed only if the hardware accepted it.
 */

#include <stdio.h>
#include <string.h>

#include "en2m_priv.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "en2m_dm";

#define EN2M_NVS_NAMESPACE "en2m_attr"

/** Compact on-flash form of a value; the struct layout itself is not stored. */
typedef struct __attribute__((packed)) {
    uint8_t type;
    int64_t raw;
} en2m_persisted_t;

static struct en2m_endpoint s_endpoints[EN2M_MAX_ENDPOINTS];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static void en2m_dm_lock_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

void en2m_dm_lock(void)
{
    en2m_dm_lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void en2m_dm_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ---- value helpers ---- */

int64_t en2m_value_as_int(const en2m_value_t *value)
{
    if (value == NULL) {
        return 0;
    }
    switch (value->type) {
    case EN2M_VAL_BOOL:
        return value->v.b ? 1 : 0;
    case EN2M_VAL_U8:
        return value->v.u8;
    case EN2M_VAL_U16:
        return value->v.u16;
    case EN2M_VAL_U32:
        return value->v.u32;
    case EN2M_VAL_I16:
        return value->v.i16;
    case EN2M_VAL_I32:
        return value->v.i32;
    case EN2M_VAL_I64:
        return value->v.i64;
    case EN2M_VAL_ENUM8:
        return value->v.e8;
    case EN2M_VAL_NULL:
    default:
        return 0;
    }
}

bool en2m_value_equal(const en2m_value_t *a, const en2m_value_t *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->type != b->type) {
        return false;
    }
    return en2m_value_as_int(a) == en2m_value_as_int(b);
}

/**
 * Re-type an incoming value to the type the attribute was declared with, so
 * callers may pass whatever integer width is natural for their driver.
 */
static en2m_value_t en2m_value_coerce(en2m_val_type_t type, const en2m_value_t *in)
{
    int64_t raw = en2m_value_as_int(in);

    switch (type) {
    case EN2M_VAL_BOOL:
        return en2m_bool(raw != 0);
    case EN2M_VAL_U8:
        return en2m_u8((uint8_t)raw);
    case EN2M_VAL_U16:
        return en2m_u16((uint16_t)raw);
    case EN2M_VAL_U32:
        return en2m_u32((uint32_t)raw);
    case EN2M_VAL_I16:
        return en2m_i16((int16_t)raw);
    case EN2M_VAL_I32:
        return en2m_i32((int32_t)raw);
    case EN2M_VAL_I64:
        return en2m_i64(raw);
    case EN2M_VAL_ENUM8:
        return en2m_enum8((uint8_t)raw);
    case EN2M_VAL_NULL:
    default:
        return *in;
    }
}

/* ---- slot lookup ---- */

struct en2m_endpoint *en2m_dm_endpoint_slot(int index)
{
    if (index < 0 || index >= EN2M_MAX_ENDPOINTS) {
        return NULL;
    }
    return &s_endpoints[index];
}

static struct en2m_endpoint *en2m_dm_find_endpoint(uint8_t endpoint_id)
{
    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (s_endpoints[i].used && s_endpoints[i].id == endpoint_id) {
            return &s_endpoints[i];
        }
    }
    return NULL;
}

struct en2m_cluster *en2m_dm_find_cluster(uint8_t endpoint_id, uint16_t cluster_id)
{
    struct en2m_endpoint *ep = en2m_dm_find_endpoint(endpoint_id);

    if (ep == NULL) {
        return NULL;
    }
    for (int i = 0; i < EN2M_MAX_CLUSTERS_PER_ENDPOINT; i++) {
        if (ep->clusters[i].used && ep->clusters[i].id == cluster_id) {
            return &ep->clusters[i];
        }
    }
    return NULL;
}

en2m_attr_slot_t *en2m_dm_find_attr(struct en2m_cluster *cluster, uint16_t attribute_id)
{
    if (cluster == NULL) {
        return NULL;
    }
    for (int i = 0; i < EN2M_MAX_ATTRIBUTES_PER_CLUSTER; i++) {
        if (cluster->attrs[i].used && cluster->attrs[i].id == attribute_id) {
            return &cluster->attrs[i];
        }
    }
    return NULL;
}

/* ---- construction ---- */

en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id)
{
    en2m_endpoint_t *ep = NULL;

    if (endpoint_id == 0 || endpoint_id == 255) {
        ESP_LOGE(TAG, "invalid endpoint id %u", endpoint_id);
        return NULL;
    }

    en2m_dm_lock();
    if (en2m_dm_find_endpoint(endpoint_id) != NULL) {
        en2m_dm_unlock();
        ESP_LOGE(TAG, "endpoint %u already exists", endpoint_id);
        return NULL;
    }
    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (!s_endpoints[i].used) {
            memset(&s_endpoints[i], 0, sizeof(s_endpoints[i]));
            s_endpoints[i].used = true;
            s_endpoints[i].id = endpoint_id;
            ep = &s_endpoints[i];
            break;
        }
    }
    en2m_dm_unlock();

    if (ep == NULL) {
        ESP_LOGE(TAG, "no free endpoint slot (EN2M_MAX_ENDPOINTS=%d)", EN2M_MAX_ENDPOINTS);
    }
    return ep;
}

en2m_endpoint_t *en2m_endpoint_get(uint8_t endpoint_id)
{
    en2m_endpoint_t *ep;

    en2m_dm_lock();
    ep = en2m_dm_find_endpoint(endpoint_id);
    en2m_dm_unlock();
    return ep;
}

/** Caller holds the lock. */
static esp_err_t en2m_dm_add_attr(struct en2m_cluster *cluster, uint16_t attribute_id,
                                  en2m_value_t default_value, bool persist)
{
    if (en2m_dm_find_attr(cluster, attribute_id) != NULL) {
        return ESP_OK;
    }
    for (int i = 0; i < EN2M_MAX_ATTRIBUTES_PER_CLUSTER; i++) {
        if (!cluster->attrs[i].used) {
            cluster->attrs[i].used = true;
            cluster->attrs[i].id = attribute_id;
            cluster->attrs[i].value = default_value;
            cluster->attrs[i].persist = persist;
            cluster->attrs[i].persist_dirty = false;
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "cluster 0x%04x is full, cannot add attribute 0x%04x", cluster->id, attribute_id);
    return ESP_ERR_NO_MEM;
}

/** The attributes every instance of a known cluster is expected to expose. */
static void en2m_dm_add_default_attrs(struct en2m_cluster *cluster)
{
    switch (cluster->id) {
    case EN2M_CLUSTER_IDENTIFY:
        en2m_dm_add_attr(cluster, EN2M_ATTR_IDENTIFY_TIME, en2m_u16(0), false);
        break;
    case EN2M_CLUSTER_ON_OFF:
        en2m_dm_add_attr(cluster, EN2M_ATTR_ON_OFF, en2m_bool(false), true);
        break;
    case EN2M_CLUSTER_LEVEL_CONTROL:
        en2m_dm_add_attr(cluster, EN2M_ATTR_CURRENT_LEVEL, en2m_u8(254), true);
        break;
    case EN2M_CLUSTER_COLOR_CONTROL:
        en2m_dm_add_attr(cluster, EN2M_ATTR_COLOR_TEMPERATURE_MIREDS, en2m_u16(300), true);
        break;
    case EN2M_CLUSTER_BOOLEAN_STATE:
        en2m_dm_add_attr(cluster, EN2M_ATTR_STATE_VALUE, en2m_bool(false), false);
        break;
    case EN2M_CLUSTER_OCCUPANCY:
        en2m_dm_add_attr(cluster, EN2M_ATTR_OCCUPANCY, en2m_bool(false), false);
        break;
    case EN2M_CLUSTER_ILLUMINANCE:
        en2m_dm_add_attr(cluster, EN2M_ATTR_MEASURED_VALUE, en2m_u32(0), false);
        break;
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT:
        en2m_dm_add_attr(cluster, EN2M_ATTR_MEASURED_VALUE, en2m_i16(0), false);
        break;
    case EN2M_CLUSTER_RELATIVE_HUMIDITY:
        en2m_dm_add_attr(cluster, EN2M_ATTR_MEASURED_VALUE, en2m_u16(0), false);
        break;
    case EN2M_CLUSTER_PRESSURE_MEASUREMENT:
        en2m_dm_add_attr(cluster, EN2M_ATTR_MEASURED_VALUE, en2m_i32(0), false);
        break;
    case EN2M_CLUSTER_SMOKE_CO:
        en2m_dm_add_attr(cluster, EN2M_ATTR_SMOKE_STATE, en2m_bool(false), false);
        en2m_dm_add_attr(cluster, EN2M_ATTR_CO_STATE, en2m_bool(false), false);
        break;
    case EN2M_CLUSTER_DOOR_LOCK:
        en2m_dm_add_attr(cluster, EN2M_ATTR_LOCK_STATE, en2m_enum8(EN2M_LOCK_LOCKED), true);
        break;
    case EN2M_CLUSTER_WINDOW_COVERING:
        en2m_dm_add_attr(cluster, EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT, en2m_u8(0), true);
        break;
    case EN2M_CLUSTER_THERMOSTAT:
        en2m_dm_add_attr(cluster, EN2M_ATTR_LOCAL_TEMPERATURE, en2m_i16(0), false);
        en2m_dm_add_attr(cluster, EN2M_ATTR_OCCUPIED_HEATING_SETPOINT, en2m_i16(2100), true);
        en2m_dm_add_attr(cluster, EN2M_ATTR_OCCUPIED_COOLING_SETPOINT, en2m_i16(2400), true);
        en2m_dm_add_attr(cluster, EN2M_ATTR_SYSTEM_MODE, en2m_enum8(EN2M_THERMOSTAT_OFF), true);
        break;
    case EN2M_CLUSTER_FAN_CONTROL:
        en2m_dm_add_attr(cluster, EN2M_ATTR_FAN_MODE, en2m_enum8(EN2M_FAN_OFF), true);
        en2m_dm_add_attr(cluster, EN2M_ATTR_PERCENT_SETTING, en2m_u8(0), true);
        break;
    case EN2M_CLUSTER_ELECTRICAL_POWER:
        en2m_dm_add_attr(cluster, EN2M_ATTR_ACTIVE_POWER_MW, en2m_i32(0), false);
        en2m_dm_add_attr(cluster, EN2M_ATTR_ENERGY_MWH, en2m_i64(0), true);
        break;
    default:
        break;
    }
}

en2m_cluster_t *en2m_cluster_create(en2m_endpoint_t *endpoint, uint16_t cluster_id)
{
    struct en2m_cluster *cluster = NULL;

    if (endpoint == NULL) {
        ESP_LOGE(TAG, "cluster 0x%04x: endpoint is NULL", cluster_id);
        return NULL;
    }

    en2m_dm_lock();
    for (int i = 0; i < EN2M_MAX_CLUSTERS_PER_ENDPOINT; i++) {
        if (endpoint->clusters[i].used && endpoint->clusters[i].id == cluster_id) {
            en2m_dm_unlock();
            return &endpoint->clusters[i];
        }
    }
    for (int i = 0; i < EN2M_MAX_CLUSTERS_PER_ENDPOINT; i++) {
        if (!endpoint->clusters[i].used) {
            cluster = &endpoint->clusters[i];
            memset(cluster, 0, sizeof(*cluster));
            cluster->used = true;
            cluster->id = cluster_id;
            cluster->endpoint_id = endpoint->id;
            en2m_dm_add_default_attrs(cluster);
            break;
        }
    }
    en2m_dm_unlock();

    if (cluster == NULL) {
        ESP_LOGE(TAG, "endpoint %u is full, cannot add cluster 0x%04x", endpoint->id, cluster_id);
    }
    return cluster;
}

en2m_cluster_t *en2m_cluster_get(en2m_endpoint_t *endpoint, uint16_t cluster_id)
{
    struct en2m_cluster *cluster = NULL;

    if (endpoint == NULL) {
        return NULL;
    }
    en2m_dm_lock();
    for (int i = 0; i < EN2M_MAX_CLUSTERS_PER_ENDPOINT; i++) {
        if (endpoint->clusters[i].used && endpoint->clusters[i].id == cluster_id) {
            cluster = &endpoint->clusters[i];
            break;
        }
    }
    en2m_dm_unlock();
    return cluster;
}

esp_err_t en2m_attribute_create(en2m_cluster_t *cluster, uint16_t attribute_id,
                                en2m_value_t default_value, bool persist)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "cluster is NULL");

    en2m_dm_lock();
    err = en2m_dm_add_attr(cluster, attribute_id, default_value, persist);
    en2m_dm_unlock();
    return err;
}

esp_err_t en2m_cluster_set_write_cb(en2m_cluster_t *cluster, en2m_attribute_write_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "cluster is NULL");
    en2m_dm_lock();
    cluster->write_cb = cb;
    cluster->write_ctx = ctx;
    en2m_dm_unlock();
    return ESP_OK;
}

esp_err_t en2m_cluster_set_read_cb(en2m_cluster_t *cluster, en2m_attribute_read_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "cluster is NULL");
    en2m_dm_lock();
    cluster->read_cb = cb;
    cluster->read_ctx = ctx;
    en2m_dm_unlock();
    return ESP_OK;
}

esp_err_t en2m_cluster_set_command_cb(en2m_cluster_t *cluster, en2m_command_handler_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "cluster is NULL");
    en2m_dm_lock();
    cluster->command_cb = cb;
    cluster->command_ctx = ctx;
    en2m_dm_unlock();
    return ESP_OK;
}

/* ---- device type recipes ---- */

esp_err_t en2m_endpoint_add_device_type(en2m_endpoint_t *endpoint, en2m_device_type_t type)
{
    ESP_RETURN_ON_FALSE(endpoint != NULL, ESP_ERR_INVALID_ARG, TAG, "endpoint is NULL");

    switch (type) {
    case EN2M_DEVICE_TYPE_ON_OFF_LIGHT:
    case EN2M_DEVICE_TYPE_ON_OFF_PLUG:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_ON_OFF);
        break;
    case EN2M_DEVICE_TYPE_DIMMABLE_LIGHT:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_ON_OFF);
        en2m_cluster_create(endpoint, EN2M_CLUSTER_LEVEL_CONTROL);
        break;
    case EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_ON_OFF);
        en2m_cluster_create(endpoint, EN2M_CLUSTER_LEVEL_CONTROL);
        en2m_cluster_create(endpoint, EN2M_CLUSTER_COLOR_CONTROL);
        break;
    case EN2M_DEVICE_TYPE_SMART_PLUG:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_ON_OFF);
        en2m_cluster_create(endpoint, EN2M_CLUSTER_ELECTRICAL_POWER);
        break;
    case EN2M_DEVICE_TYPE_CONTACT_SENSOR:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_BOOLEAN_STATE);
        break;
    case EN2M_DEVICE_TYPE_OCCUPANCY_SENSOR:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_OCCUPANCY);
        break;
    case EN2M_DEVICE_TYPE_LIGHT_SENSOR:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_ILLUMINANCE);
        break;
    case EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_TEMPERATURE_MEASUREMENT);
        break;
    case EN2M_DEVICE_TYPE_HUMIDITY_SENSOR:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_RELATIVE_HUMIDITY);
        break;
    case EN2M_DEVICE_TYPE_PRESSURE_SENSOR:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_PRESSURE_MEASUREMENT);
        break;
    case EN2M_DEVICE_TYPE_SMOKE_CO_ALARM:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_SMOKE_CO);
        break;
    case EN2M_DEVICE_TYPE_FAN:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_FAN_CONTROL);
        break;
    case EN2M_DEVICE_TYPE_WINDOW_COVERING:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_WINDOW_COVERING);
        break;
    case EN2M_DEVICE_TYPE_DOOR_LOCK:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_DOOR_LOCK);
        break;
    case EN2M_DEVICE_TYPE_THERMOSTAT:
        en2m_cluster_create(endpoint, EN2M_CLUSTER_THERMOSTAT);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

en2m_endpoint_t *en2m_endpoint_create_device(uint8_t endpoint_id, en2m_device_type_t type)
{
    en2m_endpoint_t *ep = en2m_endpoint_create(endpoint_id);

    if (ep == NULL) {
        return NULL;
    }
    if (en2m_endpoint_add_device_type(ep, type) != ESP_OK) {
        return NULL;
    }
    return ep;
}

/* ---- access ---- */

esp_err_t en2m_attribute_get(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                             en2m_value_t *out_value)
{
    en2m_attr_slot_t *slot;

    ESP_RETURN_ON_FALSE(out_value != NULL, ESP_ERR_INVALID_ARG, TAG, "out_value is NULL");

    en2m_dm_lock();
    slot = en2m_dm_find_attr(en2m_dm_find_cluster(endpoint_id, cluster_id), attribute_id);
    if (slot != NULL) {
        *out_value = slot->value;
    }
    en2m_dm_unlock();
    return (slot != NULL) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t en2m_attribute_set(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                             en2m_value_t value)
{
    en2m_attr_path_t path = {endpoint_id, cluster_id, attribute_id};
    en2m_attr_slot_t *slot;
    en2m_value_t committed;
    bool changed = false;

    en2m_dm_lock();
    slot = en2m_dm_find_attr(en2m_dm_find_cluster(endpoint_id, cluster_id), attribute_id);
    if (slot == NULL) {
        en2m_dm_unlock();
        ESP_LOGW(TAG, "set %u/0x%04x/0x%04x: no such attribute", endpoint_id, cluster_id, attribute_id);
        return ESP_ERR_NOT_FOUND;
    }
    committed = en2m_value_coerce(slot->value.type, &value);
    if (!en2m_value_equal(&slot->value, &committed)) {
        slot->value = committed;
        slot->persist_dirty = slot->persist;
        changed = true;
    }
    en2m_dm_unlock();

    if (changed) {
        en2m_model_on_change(&path, &committed);
    }
    return ESP_OK;
}

esp_err_t en2m_attribute_set_from_isr(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                                      en2m_value_t value, BaseType_t *higher_prio_task_woken)
{
    en2m_item_t item = {.kind = EN2M_ITEM_ATTR};

    item.u.attr.path.endpoint_id = endpoint_id;
    item.u.attr.path.cluster_id = cluster_id;
    item.u.attr.path.attribute_id = attribute_id;
    item.u.attr.value = value;
    return en2m_task_post(&item, true, higher_prio_task_woken);
}

esp_err_t en2m_attribute_write(uint8_t endpoint_id, uint16_t cluster_id, uint16_t attribute_id,
                               en2m_value_t value)
{
    const en2m_attr_path_t path = {endpoint_id, cluster_id, attribute_id};
    const en2m_device_config_t *cfg;
    en2m_attribute_write_cb_t cluster_cb = NULL;
    void *cluster_ctx = NULL;
    struct en2m_cluster *cluster;
    en2m_value_t target;
    en2m_attr_slot_t *slot;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    en2m_dm_lock();
    cluster = en2m_dm_find_cluster(endpoint_id, cluster_id);
    slot = en2m_dm_find_attr(cluster, attribute_id);
    if (slot == NULL) {
        en2m_dm_unlock();
        ESP_LOGW(TAG, "write %u/0x%04x/0x%04x: no such attribute", endpoint_id, cluster_id, attribute_id);
        return ESP_ERR_NOT_FOUND;
    }
    target = en2m_value_coerce(slot->value.type, &value);
    cluster_cb = cluster->write_cb;
    cluster_ctx = cluster->write_ctx;
    en2m_dm_unlock();

    if (cluster_cb != NULL) {
        err = cluster_cb(&path, &target, cluster_ctx);
    }
    cfg = en2m_model_config();
    if (err == ESP_ERR_NOT_SUPPORTED && cfg != NULL && cfg->attribute_write != NULL) {
        err = cfg->attribute_write(&path, &target, cfg->user_ctx);
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "write %u/0x%04x/0x%04x rejected: %s", endpoint_id, cluster_id, attribute_id,
                 esp_err_to_name(err));
        return err;
    }

    return en2m_attribute_set(endpoint_id, cluster_id, attribute_id, target);
}

/* ---- convenience reporters ---- */

esp_err_t en2m_report_on_off(uint8_t endpoint_id, bool on)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(on));
}

esp_err_t en2m_report_level(uint8_t endpoint_id, uint8_t level)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_LEVEL_CONTROL, EN2M_ATTR_CURRENT_LEVEL,
                              en2m_u8(level));
}

esp_err_t en2m_report_color_temperature(uint8_t endpoint_id, uint16_t mireds)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_COLOR_CONTROL, EN2M_ATTR_COLOR_TEMPERATURE_MIREDS,
                              en2m_u16(mireds));
}

esp_err_t en2m_report_temperature(uint8_t endpoint_id, int16_t centi_celsius)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_TEMPERATURE_MEASUREMENT, EN2M_ATTR_MEASURED_VALUE,
                              en2m_i16(centi_celsius));
}

esp_err_t en2m_report_humidity(uint8_t endpoint_id, uint16_t centi_percent)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_RELATIVE_HUMIDITY, EN2M_ATTR_MEASURED_VALUE,
                              en2m_u16(centi_percent));
}

esp_err_t en2m_report_pressure(uint8_t endpoint_id, int32_t deci_hpa)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_PRESSURE_MEASUREMENT, EN2M_ATTR_MEASURED_VALUE,
                              en2m_i32(deci_hpa));
}

esp_err_t en2m_report_illuminance(uint8_t endpoint_id, uint32_t lux)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_ILLUMINANCE, EN2M_ATTR_MEASURED_VALUE,
                              en2m_u32(lux));
}

esp_err_t en2m_report_boolean_state(uint8_t endpoint_id, bool state_value)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_BOOLEAN_STATE, EN2M_ATTR_STATE_VALUE,
                              en2m_bool(state_value));
}

esp_err_t en2m_report_occupancy(uint8_t endpoint_id, bool occupied)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_OCCUPANCY, EN2M_ATTR_OCCUPANCY,
                              en2m_bool(occupied));
}

esp_err_t en2m_report_smoke(uint8_t endpoint_id, bool alarm)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_SMOKE_CO, EN2M_ATTR_SMOKE_STATE, en2m_bool(alarm));
}

esp_err_t en2m_report_co(uint8_t endpoint_id, bool alarm)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_SMOKE_CO, EN2M_ATTR_CO_STATE, en2m_bool(alarm));
}

esp_err_t en2m_report_lock_state(uint8_t endpoint_id, en2m_lock_state_t state)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_DOOR_LOCK, EN2M_ATTR_LOCK_STATE,
                              en2m_enum8((uint8_t)state));
}

esp_err_t en2m_report_cover_position(uint8_t endpoint_id, uint8_t closed_percent)
{
    if (closed_percent > 100) {
        closed_percent = 100;
    }
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_WINDOW_COVERING,
                              EN2M_ATTR_CURRENT_POSITION_LIFT_PERCENT, en2m_u8(closed_percent));
}

esp_err_t en2m_report_fan(uint8_t endpoint_id, en2m_fan_mode_t mode, uint8_t percent)
{
    esp_err_t err = en2m_attribute_set(endpoint_id, EN2M_CLUSTER_FAN_CONTROL, EN2M_ATTR_FAN_MODE,
                                       en2m_enum8((uint8_t)mode));
    if (percent > 100) {
        percent = 100;
    }
    if (err == ESP_OK) {
        err = en2m_attribute_set(endpoint_id, EN2M_CLUSTER_FAN_CONTROL, EN2M_ATTR_PERCENT_SETTING,
                                 en2m_u8(percent));
    }
    return err;
}

esp_err_t en2m_report_power(uint8_t endpoint_id, int32_t milliwatts)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_ELECTRICAL_POWER, EN2M_ATTR_ACTIVE_POWER_MW,
                              en2m_i32(milliwatts));
}

esp_err_t en2m_report_energy(uint8_t endpoint_id, int64_t milliwatt_hours)
{
    return en2m_attribute_set(endpoint_id, EN2M_CLUSTER_ELECTRICAL_POWER, EN2M_ATTR_ENERGY_MWH,
                              en2m_i64(milliwatt_hours));
}

/* ---- read refresh ---- */

/**
 * Walk every attribute and pull a live value for the ones that expose a read
 * callback. Callbacks run without the store lock held so that they may touch
 * any en2m API, which is why the slot is re-resolved for each attribute.
 */
void en2m_dm_refresh(void)
{
    const en2m_device_config_t *cfg = en2m_model_config();

    for (int e = 0; e < EN2M_MAX_ENDPOINTS; e++) {
        for (int c = 0; c < EN2M_MAX_CLUSTERS_PER_ENDPOINT; c++) {
            for (int a = 0; a < EN2M_MAX_ATTRIBUTES_PER_CLUSTER; a++) {
                en2m_attr_path_t path;
                en2m_attribute_read_cb_t cb;
                void *ctx;
                en2m_value_t value;
                esp_err_t err;

                en2m_dm_lock();
                if (!s_endpoints[e].used || !s_endpoints[e].clusters[c].used ||
                    !s_endpoints[e].clusters[c].attrs[a].used) {
                    en2m_dm_unlock();
                    continue;
                }
                path.endpoint_id = s_endpoints[e].id;
                path.cluster_id = s_endpoints[e].clusters[c].id;
                path.attribute_id = s_endpoints[e].clusters[c].attrs[a].id;
                value = s_endpoints[e].clusters[c].attrs[a].value;
                cb = s_endpoints[e].clusters[c].read_cb;
                ctx = s_endpoints[e].clusters[c].read_ctx;
                en2m_dm_unlock();

                if (cb == NULL && cfg != NULL) {
                    cb = cfg->attribute_read;
                    ctx = cfg->user_ctx;
                }
                if (cb == NULL) {
                    continue;
                }
                err = cb(&path, &value, ctx);
                if (err == ESP_OK) {
                    en2m_attribute_set(path.endpoint_id, path.cluster_id, path.attribute_id, value);
                }
            }
        }
    }
}

/* ---- persistence ---- */

static void en2m_dm_persist_key(const en2m_attr_path_t *path, char out[16])
{
    snprintf(out, 16, "%u_%04x_%04x", path->endpoint_id, path->cluster_id, path->attribute_id);
}

void en2m_dm_restore(void)
{
    nvs_handle_t nvs;

    if (nvs_open(EN2M_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    en2m_dm_lock();
    for (int e = 0; e < EN2M_MAX_ENDPOINTS; e++) {
        if (!s_endpoints[e].used) {
            continue;
        }
        for (int c = 0; c < EN2M_MAX_CLUSTERS_PER_ENDPOINT; c++) {
            if (!s_endpoints[e].clusters[c].used) {
                continue;
            }
            for (int a = 0; a < EN2M_MAX_ATTRIBUTES_PER_CLUSTER; a++) {
                en2m_attr_slot_t *slot = &s_endpoints[e].clusters[c].attrs[a];
                en2m_persisted_t stored;
                size_t size = sizeof(stored);
                en2m_attr_path_t path;
                char key[16];
                en2m_value_t value;

                if (!slot->used || !slot->persist) {
                    continue;
                }
                path.endpoint_id = s_endpoints[e].id;
                path.cluster_id = s_endpoints[e].clusters[c].id;
                path.attribute_id = slot->id;
                en2m_dm_persist_key(&path, key);
                if (nvs_get_blob(nvs, key, &stored, &size) != ESP_OK || size != sizeof(stored)) {
                    continue;
                }
                value = en2m_i64(stored.raw);
                slot->value = en2m_value_coerce((en2m_val_type_t)stored.type, &value);
                slot->persist_dirty = false;
            }
        }
    }
    en2m_dm_unlock();
    nvs_close(nvs);
}

void en2m_dm_flush_persist(void)
{
    nvs_handle_t nvs = 0;
    bool opened = false;
    bool wrote = false;

    for (int e = 0; e < EN2M_MAX_ENDPOINTS; e++) {
        for (int c = 0; c < EN2M_MAX_CLUSTERS_PER_ENDPOINT; c++) {
            for (int a = 0; a < EN2M_MAX_ATTRIBUTES_PER_CLUSTER; a++) {
                en2m_attr_slot_t *slot;
                en2m_persisted_t stored;
                en2m_attr_path_t path;
                char key[16];

                en2m_dm_lock();
                slot = &s_endpoints[e].clusters[c].attrs[a];
                if (!s_endpoints[e].used || !s_endpoints[e].clusters[c].used || !slot->used ||
                    !slot->persist_dirty) {
                    en2m_dm_unlock();
                    continue;
                }
                path.endpoint_id = s_endpoints[e].id;
                path.cluster_id = s_endpoints[e].clusters[c].id;
                path.attribute_id = slot->id;
                stored.type = (uint8_t)slot->value.type;
                stored.raw = en2m_value_as_int(&slot->value);
                slot->persist_dirty = false;
                en2m_dm_unlock();

                if (!opened) {
                    if (nvs_open(EN2M_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
                        ESP_LOGW(TAG, "nvs_open failed, attribute state will not survive a reboot");
                        return;
                    }
                    opened = true;
                }
                en2m_dm_persist_key(&path, key);
                if (nvs_set_blob(nvs, key, &stored, sizeof(stored)) == ESP_OK) {
                    wrote = true;
                }
            }
        }
    }

    if (opened) {
        if (wrote) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}
