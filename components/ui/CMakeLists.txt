idf_component_register(
    SRCS "ui_manager.c" "ui_pages.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES freertos log
)
