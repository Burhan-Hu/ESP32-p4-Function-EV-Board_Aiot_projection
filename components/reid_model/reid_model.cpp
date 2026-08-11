/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "reid_model.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "dl_image_preprocessor.hpp"
#include "fbs_loader.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "reid_model";

static dl::Model *s_model = nullptr;
static dl::image::ImagePreprocessor *s_preprocessor = nullptr;

/* ImageNet normalization used during training.
 * ImagePreprocessor expects mean/std in [0, 255] range. */
static constexpr std::array<float, 3> REID_MEAN = {0.485f * 255.0f, 0.456f * 255.0f, 0.406f * 255.0f};
static constexpr std::array<float, 3> REID_STD  = {0.229f * 255.0f, 0.224f * 255.0f, 0.225f * 255.0f};

int reid_model_init(void)
{
    if (s_model != nullptr) {
        return 0;
    }

    s_model = new dl::Model("reid_model", "osnet_upperbody.espdl", fbs::MODEL_LOCATION_IN_FLASH_PARTITION);
    if (s_model == nullptr) {
        ESP_LOGE(TAG, "failed to create Re-ID model");
        return -1;
    }
    s_model->minimize();

    auto input = s_model->get_input("");
    if (input == nullptr) {
        ESP_LOGE(TAG, "model input not found");
        delete s_model;
        s_model = nullptr;
        return -1;
    }

    auto input_shape = input->get_shape();
    if (input_shape.size() != 4 || input_shape[0] != 1 ||
        input_shape[1] != REID_INPUT_H || input_shape[2] != REID_INPUT_W || input_shape[3] != 3) {
        ESP_LOGE(TAG, "unexpected input shape: %d %d %d %d",
                 input_shape[0], input_shape[1], input_shape[2], input_shape[3]);
        delete s_model;
        s_model = nullptr;
        return -1;
    }

    s_preprocessor = new dl::image::ImagePreprocessor(s_model, REID_MEAN, REID_STD);
    if (s_preprocessor == nullptr) {
        ESP_LOGE(TAG, "failed to create image preprocessor");
        delete s_model;
        s_model = nullptr;
        return -1;
    }

    ESP_LOGI(TAG, "OSNet Re-ID model loaded, input %dx%d, output %d-d",
             REID_INPUT_W, REID_INPUT_H, REID_FEATURE_DIM);
    return 0;
}

bool reid_model_extract(const uint8_t *rgb565_buf,
                        int frame_w, int frame_h,
                        int x1, int y1, int x2, int y2,
                        float *feature_out)
{
    if (s_model == nullptr || s_preprocessor == nullptr ||
        rgb565_buf == nullptr || feature_out == nullptr) {
        return false;
    }

    /* Clamp bbox. */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > frame_w) x2 = frame_w;
    if (y2 > frame_h) y2 = frame_h;
    if (x2 <= x1 || y2 <= y1) {
        ESP_LOGW(TAG, "invalid crop bbox: (%d,%d,%d,%d)", x1, y1, x2, y2);
        return false;
    }

    /* Use upper 60% of the person bbox as the Re-ID input. */
    int h_full = y2 - y1;
    int y2_top = y1 + (int)(h_full * 0.6f);
    if (y2_top <= y1) y2_top = y1 + 1;

    dl::image::img_t src = {
        .data = (void *)rgb565_buf,
        .width = (uint16_t)frame_w,
        .height = (uint16_t)frame_h,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    std::vector<int> crop_area = {x1, y1, x2, y2_top};
    s_preprocessor->preprocess(src, crop_area);

    s_model->run();

    dl::TensorBase *output_tensor = s_model->get_outputs()["feature"];
    if (output_tensor == nullptr) {
        ESP_LOGE(TAG, "model output 'feature' not found");
        return false;
    }

    if (output_tensor->get_dtype() == dl::DATA_TYPE_FLOAT) {
        float *raw = output_tensor->get_element_ptr<float>();
        std::memcpy(feature_out, raw, REID_FEATURE_DIM * sizeof(float));
    } else if (output_tensor->get_dtype() == dl::DATA_TYPE_INT8) {
        int8_t *raw = output_tensor->get_element_ptr<int8_t>();
        float scale = DL_SCALE(output_tensor->exponent);
        for (int i = 0; i < REID_FEATURE_DIM; ++i) {
            feature_out[i] = raw[i] * scale;
        }
    } else {
        ESP_LOGE(TAG, "unsupported output dtype: %s", output_tensor->get_dtype_string());
        return false;
    }

    /* L2 normalize. */
    float sum = 0.0f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        sum += feature_out[i] * feature_out[i];
    }
    float norm = std::sqrt(sum) + 1e-8f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        feature_out[i] /= norm;
    }

    return true;
}

float reid_cosine_similarity(const float *a, const float *b)
{
    float dot = 0.0f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        dot += a[i] * b[i];
    }
    return dot;
}
