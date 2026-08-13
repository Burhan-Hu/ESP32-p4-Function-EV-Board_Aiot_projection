idf_component_register(
    SRCS "family_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json mbedtls heap esp_hw_support wifi_conn
)
