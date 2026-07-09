idf_component_register(SRCS "human_detect.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES led_controller esp_mm)

idf_component_optional_requires(PRIVATE pedestrian_detect espressif__pedestrian_detect)
