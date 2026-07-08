idf_component_register(SRCS "mipi_isp_dsi_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c esp_driver_jpeg dsi_init sensor_init driver wifi_conn slave_ota esp_http_client mbedtls audio wake_word cloud_api pir camera_capture
                       )
