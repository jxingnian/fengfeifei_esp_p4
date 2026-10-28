idf_component_register(
    SRCS "xn_device_connector.c"
         "xn_pairing_manager.c"
         "xn_auth_manager.c"
         "xn_connection_manager.c"
         "xn_message_router.c"
         "xn_config_sync.c"
    INCLUDE_DIRS "include"
    # 组件源码直接使用 WebSocket 客户端 API，必须显式声明公共依赖。
    REQUIRES nvs_flash esp_http_client esp_websocket_client cjson
)
