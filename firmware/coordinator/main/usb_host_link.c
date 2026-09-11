#include "usb_host_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_link";
static usb_line_cb_t s_cb;
static void *s_user;

static void usb_rx_task(void *arg)
{
    (void)arg;
    char line[1024];
    size_t n = 0;
    uint8_t ch;

    while (1) {
        int r = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (r <= 0) {
            continue;
        }
        if (ch == '\n') {
            line[n] = 0;
            if (n > 0 && s_cb) {
                /* trim CR */
                if (n > 0 && line[n - 1] == '\r') {
                    line[n - 1] = 0;
                }
                s_cb(line, s_user);
            }
            n = 0;
        } else if (ch != '\r') {
            if (n + 1 < sizeof(line)) {
                line[n++] = (char)ch;
            } else {
                n = 0;
            }
        }
    }
}

esp_err_t usb_host_link_start(usb_line_cb_t on_line, void *user)
{
    s_cb = on_line;
    s_user = user;

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "USB Serial/JTAG host link ready");
    return ESP_OK;
}

void usb_host_link_write(const char *line)
{
    if (!line) {
        return;
    }
    size_t len = strlen(line);
    usb_serial_jtag_write_bytes(line, len, pdMS_TO_TICKS(100));
    const char nl = '\n';
    usb_serial_jtag_write_bytes(&nl, 1, pdMS_TO_TICKS(20));
}

void usb_host_link_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    usb_host_link_write(buf);
}
