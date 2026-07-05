idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver" "esp_codec_dev" "esp_common" "heap"
)
