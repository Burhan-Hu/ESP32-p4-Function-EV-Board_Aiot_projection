/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_api_types.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "slave_ota.h"

static const char *TAG = "slave_ota";

#ifndef SLAVE_OTA_CHUNK_SIZE
#define SLAVE_OTA_CHUNK_SIZE 1500
#endif

#define SLAVE_OTA_PARTITION_LABEL "slave_fw"

/* Parse image header and calculate total firmware size */
static esp_err_t parse_image_size(const esp_partition_t *partition, size_t *firmware_size)
{
    esp_image_header_t image_header;
    esp_image_segment_header_t segment_header;
    esp_err_t ret;
    size_t offset = 0;
    size_t total_size = 0;

    ret = esp_partition_read(partition, offset, &image_header, sizeof(image_header));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read image header: %s", esp_err_to_name(ret));
        return ret;
    }

    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "Invalid image magic: 0x%02X (expected 0x%02X)",
                 image_header.magic, ESP_IMAGE_HEADER_MAGIC);
        ESP_LOGE(TAG, "Partition '%s' does not contain a valid ESP32 firmware image",
                 partition->label);
        return ESP_ERR_INVALID_ARG;
    }

    total_size = sizeof(image_header);
    offset = sizeof(image_header);

    for (int i = 0; i < image_header.segment_count; i++) {
        ret = esp_partition_read(partition, offset, &segment_header, sizeof(segment_header));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read segment %d header: %s", i, esp_err_to_name(ret));
            return ret;
        }
        total_size += sizeof(segment_header) + segment_header.data_len;
        offset += sizeof(segment_header) + segment_header.data_len;
    }

    /* Align to 16 bytes, add checksum byte */
    total_size += (16 - (total_size % 16)) % 16;
    total_size += 1;

    /* Add SHA256 hash if appended */
    if (image_header.hash_appended) {
        total_size += (16 - (total_size % 16)) % 16;
        total_size += 32;
    }

    *firmware_size = total_size;
    ESP_LOGI(TAG, "Firmware image size: %u bytes", (unsigned int)total_size);
    return ESP_OK;
}

/* Read app version from the firmware image in the partition */
static esp_err_t get_new_firmware_version(const esp_partition_t *partition, char *version, size_t version_len)
{
    esp_image_header_t image_header;
    esp_image_segment_header_t segment_header;
    esp_app_desc_t app_desc;
    esp_err_t ret;

    ret = esp_partition_read(partition, 0, &image_header, sizeof(image_header));
    if (ret != ESP_OK) {
        return ret;
    }

    size_t app_desc_offset = sizeof(image_header) + sizeof(segment_header);
    ret = esp_partition_read(partition, app_desc_offset, &app_desc, sizeof(app_desc));
    if (ret == ESP_OK) {
        strncpy(version, app_desc.version, version_len - 1);
        version[version_len - 1] = '\0';
    } else {
        strncpy(version, "unknown", version_len - 1);
        version[version_len - 1] = '\0';
    }
    return ESP_OK;
}

/* Check if partition first 1KB is all 0xFF (empty/unflashed) */
static esp_err_t check_partition_has_firmware(const esp_partition_t *partition)
{
    uint8_t buffer[256];
    size_t total_checked = 0;

    while (total_checked < 1024 && total_checked < partition->size) {
        size_t check_size = (1024 - total_checked > sizeof(buffer)) ? sizeof(buffer) : (1024 - total_checked);
        esp_err_t ret = esp_partition_read(partition, total_checked, buffer, check_size);
        if (ret != ESP_OK) {
            return ret;
        }
        for (size_t i = 0; i < check_size; i++) {
            if (buffer[i] != 0xFF) {
                return ESP_OK;
            }
        }
        total_checked += check_size;
    }
    ESP_LOGE(TAG, "Partition '%s' appears empty (first 1KB is 0xFF). Did you flash the slave firmware?",
             partition->label);
    return ESP_ERR_NOT_FOUND;
}

static void restart_host(void)
{
    ESP_LOGW(TAG, "Restarting host in 2 seconds to resync with co-processor...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* Static buffer to avoid large stack allocation in main task */
static uint8_t s_ota_chunk[SLAVE_OTA_CHUNK_SIZE];

esp_err_t slave_ota_perform_if_needed(void)
{
    const esp_partition_t *partition = NULL;
    esp_err_t ret = ESP_OK;
    uint8_t *chunk = s_ota_chunk;
    size_t offset = 0;
    size_t firmware_size = 0;
    char new_version[32] = {0};
    esp_hosted_coprocessor_fwver_t current_version = {0};

    ESP_LOGI(TAG, "Checking C6 slave firmware...");

    /* Find the slave firmware partition */
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                         SLAVE_OTA_PARTITION_LABEL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition '%s' not found in partition table", SLAVE_OTA_PARTITION_LABEL);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Found partition '%s', size: %" PRIu32 " bytes", partition->label, partition->size);

    ret = check_partition_has_firmware(partition);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = parse_image_size(partition, &firmware_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse firmware image: %s", esp_err_to_name(ret));
        return ret;
    }

    if (firmware_size == 0 || firmware_size > partition->size) {
        ESP_LOGE(TAG, "Invalid firmware size: %u (partition size: %" PRIu32 ")",
                 (unsigned int)firmware_size, partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    get_new_firmware_version(partition, new_version, sizeof(new_version));
    ESP_LOGI(TAG, "New slave firmware version in partition: %s", new_version);

    /* Get currently running slave firmware version */
    ret = esp_hosted_get_coprocessor_fwversion(&current_version);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Current slave firmware version: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 current_version.major1, current_version.minor1, current_version.patch1);

        /* Skip OTA if versions match */
        if (current_version.major1 == 0 && current_version.minor1 == 0 && current_version.patch1 == 0) {
            ESP_LOGW(TAG, "Slave reports 0.0.0 - slave firmware is missing/invalid, proceeding with OTA");
        } else {
            char current_version_str[32];
            snprintf(current_version_str, sizeof(current_version_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                     current_version.major1, current_version.minor1, current_version.patch1);
            if (strcmp(new_version, current_version_str) == 0) {
                ESP_LOGI(TAG, "Slave firmware is already up to date (%s), skipping OTA", current_version_str);
                return ESP_OK;
            }
            ESP_LOGI(TAG, "Version differs (%s -> %s), proceeding with OTA",
                     current_version_str, new_version);
        }
    } else {
        ESP_LOGW(TAG, "Could not get current slave version (%s), proceeding with OTA",
                 esp_err_to_name(ret));
    }

    /* Begin OTA session */
    ESP_LOGI(TAG, "Starting slave OTA, firmware size: %u bytes", (unsigned int)firmware_size);
    ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin slave OTA: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Stream firmware image to slave in chunks */
    uint32_t total_bytes_sent = 0;
    uint32_t chunk_count = 0;
    offset = 0;

    while (offset < firmware_size) {
        size_t bytes_to_read = (firmware_size - offset > SLAVE_OTA_CHUNK_SIZE)
                                   ? SLAVE_OTA_CHUNK_SIZE
                                   : (firmware_size - offset);

        ret = esp_partition_read(partition, offset, chunk, bytes_to_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read partition at offset %u: %s",
                     (unsigned int)offset, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }

        ret = esp_hosted_slave_ota_write(chunk, bytes_to_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA chunk %" PRIu32 ": %s",
                     chunk_count, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }

        total_bytes_sent += bytes_to_read;
        offset += bytes_to_read;
        chunk_count++;

        if (chunk_count % 50 == 0) {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "/%u bytes (%.1f%%)",
                     total_bytes_sent, (unsigned int)firmware_size,
                     (float)total_bytes_sent * 100.0f / (float)firmware_size);
        }
    }

    /* Finalise OTA session */
    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to end slave OTA: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Slave OTA data transfer completed (%" PRIu32 " bytes)", total_bytes_sent);

    /* Activate new firmware. Even if the running slave firmware is old and
     * returns an error, attempting activation is harmless and will work once
     * the slave side supports the RPC. */
    ret = esp_hosted_slave_ota_activate();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "New slave firmware activated, slave will reboot");
    } else {
        ESP_LOGW(TAG, "esp_hosted_slave_ota_activate() returned %s - slave may reboot automatically or on next host reset",
                 esp_err_to_name(ret));
    }

    /* Restart host to re-sync with the (possibly rebooted) slave */
    restart_host();

    /* Should never reach here */
    return ESP_OK;
}
