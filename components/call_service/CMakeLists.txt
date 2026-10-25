idf_component_register(
    SRCS "call_service.c"
    INCLUDE_DIRS "include"
    REQUIRES cloud_gateway voice_board_audio voice_service json freertos
)
