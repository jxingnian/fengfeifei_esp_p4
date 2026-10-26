idf_component_register(
    SRCS "cloud_gateway.c" "cloud_gateway_storage.c" "cloud_gateway_web.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_event esp_netif esp_timer esp_system esp_hw_support esp_app_format
        esp-tls esp_websocket_client nvs_flash cjson wifi_provisioning lwip
    REQUIRES esp_http_server freertos
)
