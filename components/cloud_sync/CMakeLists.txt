idf_component_register(SRCS "cloud_sync.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client json lwip esp_netif family_events mbedtls
                       )
