idf_component_register(
    SRCS "call_service.c" "call_media.c" "call_media_livekit.c"
    INCLUDE_DIRS "include"
    REQUIRES cloud_gateway voice_board_audio voice_service voice_ai_service cjson freertos livekit
        esp_video esp_capture av_render esp_audio_codec esp_video_codec esp_timer
)
