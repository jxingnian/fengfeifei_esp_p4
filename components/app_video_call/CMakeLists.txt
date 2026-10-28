idf_component_register(
    SRCS "video_call.c"
    INCLUDE_DIRS "."
    REQUIRES svc_audio svc_video_capture
)
