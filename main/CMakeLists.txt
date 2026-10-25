idf_component_register(
    SRCS
        "app_main.c"
        "p4_c5_wifi.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_event
        nvs_flash
    REQUIRES
        wifi_provisioning
        voice_service
        cloud_gateway
        call_service
)
