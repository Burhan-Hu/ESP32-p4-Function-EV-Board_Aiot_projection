idf_component_register(SRCS "camera_capture.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_cam esp_driver_jpeg esp_mm
                       )
