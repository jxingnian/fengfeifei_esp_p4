idf_component_register(
    SRCS "xn_device_connector.c"
         "xn_pairing_manager.c"
         "xn_auth_manager.c"
         "xn_connection_manager.c"
         "xn_message_router.c"
         "xn_config_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_client json
)
