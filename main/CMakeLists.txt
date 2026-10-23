idf_component_register(
    SRCS
        "app_main.c"
        "p4_c5_wifi.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_event
        esp_hosted
        esp_netif
        esp_wifi
        freertos
        lwip
        nvs_flash
)
