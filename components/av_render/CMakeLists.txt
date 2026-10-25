
idf_component_register(
    SRC_DIRS "src" "render_impl"
    INCLUDE_DIRS "include"
    REQUIRES esp_codec_dev esp_lcd
    PRIV_REQUIRES esp_timer
)

# Add color convert ASM optimized lib
# Headless build: scalar fallback, no LCD or prebuilt conversion archive.
