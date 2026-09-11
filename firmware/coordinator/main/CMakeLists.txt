idf_component_register(
    SRCS "main.c" "usb_host_link.c"
    INCLUDE_DIRS "."
    REQUIRES en2m json driver esp_timer nvs_flash
)
