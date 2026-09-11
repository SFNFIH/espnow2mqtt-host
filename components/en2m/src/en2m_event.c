/**
 * @file en2m_event.c
 * @brief Event publication on the ESP-IDF default event loop.
 */

#include "en2m_event.h"
#include "esp_log.h"

static const char *TAG = "en2m_event";

ESP_EVENT_DEFINE_BASE(EN2M_EVENT);

/**
 * The default loop is normally created by en2m_mesh_init, but a handler may be
 * registered before the stack starts, so make sure the loop exists either way.
 */
static esp_err_t en2m_event_loop_ready(void)
{
    esp_err_t err = esp_event_loop_create_default();
    return (err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}

esp_err_t en2m_event_handler_register(int32_t event_id, esp_event_handler_t handler, void *handler_arg)
{
    esp_err_t err = en2m_event_loop_ready();
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_register(EN2M_EVENT, event_id, handler, handler_arg);
}

esp_err_t en2m_event_handler_unregister(int32_t event_id, esp_event_handler_t handler)
{
    return esp_event_handler_unregister(EN2M_EVENT, event_id, handler);
}

void en2m_event_post(en2m_event_id_t id, const void *data, size_t size)
{
    esp_err_t err;

    if (en2m_event_loop_ready() != ESP_OK) {
        return;
    }
    err = esp_event_post(EN2M_EVENT, (int32_t)id, (void *)data, size, 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "post %d failed: %s", (int)id, esp_err_to_name(err));
    }
}
