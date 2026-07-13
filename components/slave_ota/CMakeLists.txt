idf_component_register(
    SRCS "slave_ota.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_partition" "esp_hosted" "bootloader_support" "esp_app_format"
)

# --- Flash bundled C6 slave firmware into the 'slave_fw' partition ------------
set(SLAVE_FW_PARTITION_NAME "slave_fw")

# Locate partition offset from the project partition table
file(READ "${CMAKE_SOURCE_DIR}/partitions.csv" PARTITION_TABLE_CONTENT)
string(REGEX MATCH "${SLAVE_FW_PARTITION_NAME}[^,]*,[^,]*,[^,]*,[ ]*([^,]*)" PARTITION_MATCH "${PARTITION_TABLE_CONTENT}")
if(PARTITION_MATCH)
    string(REGEX REPLACE "${SLAVE_FW_PARTITION_NAME}[^,]*,[^,]*,[^,]*,[ ]*([^,]*)" "\\1" SLAVE_FW_OFFSET "${PARTITION_MATCH}")
    string(STRIP "${SLAVE_FW_OFFSET}" SLAVE_FW_OFFSET)
    message(STATUS "slave_ota: found '${SLAVE_FW_PARTITION_NAME}' partition at offset ${SLAVE_FW_OFFSET}")
else()
    # Auto-calculate offset if not explicitly set in CSV (factory partition size 2M)
    set(SLAVE_FW_OFFSET "0x300000")
    message(WARNING "slave_ota: could not parse '${SLAVE_FW_PARTITION_NAME}' offset from partitions.csv, using fallback ${SLAVE_FW_OFFSET}")
endif()

# Pick the newest .bin placed in slave_fw_bin/ (user can copy network_adapter.bin here)
file(GLOB SLAVE_FW_BIN_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/slave_fw_bin/*.bin")

if(SLAVE_FW_BIN_FILES)
    list(SORT SLAVE_FW_BIN_FILES)
    list(GET SLAVE_FW_BIN_FILES -1 SLAVE_FW_BIN_SELECTED)
    get_filename_component(SLAVE_FW_BIN_NAME "${SLAVE_FW_BIN_SELECTED}" NAME)
    message(STATUS "slave_ota: selected slave firmware '${SLAVE_FW_BIN_NAME}' for partition flash")

    idf_build_get_property(PYTHON PYTHON)

    # Copy selected binary to build dir so it is available at flash time
    add_custom_target(slave_ota_select_firmware
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLAVE_FW_BIN_SELECTED}" "${CMAKE_CURRENT_BINARY_DIR}/slave_fw_selected.bin"
        COMMENT "slave_ota: staging slave firmware ${SLAVE_FW_BIN_NAME}"
        VERBATIM
    )

    add_custom_target(slave_ota_flash_firmware
        DEPENDS slave_ota_select_firmware
        COMMAND ${CMAKE_COMMAND}
            -D PYTHON=${PYTHON}
            -D SLAVE_FW_OFFSET=${SLAVE_FW_OFFSET}
            -D SLAVE_FW_SELECTED=${CMAKE_CURRENT_BINARY_DIR}/slave_fw_selected.bin
            -P "${CMAKE_CURRENT_SOURCE_DIR}/flash_slave.cmake"
        COMMENT "slave_ota: flashing C6 slave firmware to partition ${SLAVE_FW_PARTITION_NAME}"
        VERBATIM
    )

    idf_build_get_property(project_name PROJECT_NAME)
    add_dependencies(slave_ota_flash_firmware ${project_name}.elf)
    add_dependencies(flash slave_ota_flash_firmware)
else()
    message(WARNING "slave_ota: no .bin files in slave_fw_bin/. C6 slave firmware will NOT be flashed to host partition!")
endif()
