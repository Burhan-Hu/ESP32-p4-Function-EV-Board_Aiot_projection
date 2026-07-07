idf_component_register(
    SRCS "cloud_api.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_http_client" "json" "mbedtls" "esp_common"
)

target_link_libraries(${COMPONENT_LIB} PUBLIC m)
