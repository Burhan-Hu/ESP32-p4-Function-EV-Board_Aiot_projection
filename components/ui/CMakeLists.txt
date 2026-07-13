idf_component_register(
    SRCS "ui_manager.c" "ui_pages.c" "ui_lvgl.c" "ui_font_custom.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES freertos log esp_mm
)
