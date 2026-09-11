#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usb_line_cb_t)(const char *line, void *user);

esp_err_t usb_host_link_start(usb_line_cb_t on_line, void *user);
void usb_host_link_write(const char *line);
void usb_host_link_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
