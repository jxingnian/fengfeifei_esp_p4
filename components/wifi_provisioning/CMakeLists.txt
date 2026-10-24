# Wi-Fi 网页配网组件使用标准 ESP-IDF Wi-Fi API，因此既可运行在本机 Wi-Fi
# 芯片，也可在本项目中通过 esp_wifi_remote 将调用转发到 ESP32-C5。
idf_component_register(
    SRCS
        "wifi_provisioning.c"
        "wifi_provisioning_storage.c"
        "wifi_provisioning_web.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        esp_event
        esp_hosted
        esp_http_server
        esp_netif
        esp_wifi
        freertos
        lwip
        nvs_flash
)
