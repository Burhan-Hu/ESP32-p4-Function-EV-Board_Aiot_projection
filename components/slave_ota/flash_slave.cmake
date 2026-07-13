# Helper script to flash the C6 slave firmware, respecting the ESPPORT
# environment variable (same as the main IDF flash target).

if(NOT DEFINED PYTHON)
    message(FATAL_ERROR "PYTHON must be defined")
endif()
if(NOT DEFINED SLAVE_FW_OFFSET)
    message(FATAL_ERROR "SLAVE_FW_OFFSET must be defined")
endif()
if(NOT DEFINED SLAVE_FW_SELECTED)
    message(FATAL_ERROR "SLAVE_FW_SELECTED must be defined")
endif()

set(ESPPORT $ENV{ESPPORT})

set(esptool_args
    --chip esp32p4
    --before default_reset
    --after hard_reset
    write_flash
    --force
    --flash_mode dio
    --flash_size 16MB
    --flash_freq 80m
    ${SLAVE_FW_OFFSET}
    "${SLAVE_FW_SELECTED}"
)

if(ESPPORT)
    list(INSERT esptool_args 0 --port ${ESPPORT})
    message(STATUS "slave_ota: using serial port ${ESPPORT} from ESPPORT environment variable")
else()
    message(STATUS "slave_ota: ESPPORT not set, esptool will auto-detect serial port")
endif()

execute_process(
    COMMAND ${PYTHON} -m esptool ${esptool_args}
    RESULT_VARIABLE flash_result
)

if(NOT flash_result EQUAL 0)
    message(FATAL_ERROR "slave_ota: failed to flash C6 slave firmware (exit code ${flash_result})")
endif()
