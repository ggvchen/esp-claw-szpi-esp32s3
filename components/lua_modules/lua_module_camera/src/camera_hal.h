/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_FORMAT_DESCRIPTION_MAX 32

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    char pixel_format_str[5];
} camera_stream_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    char pixel_format_str[5];
    size_t frame_bytes;
    int64_t timestamp_us;
} camera_frame_info_t;

/**
 * @brief Optional parameters for camera_open().
 *
 * A zero field means "let the driver decide". When any field is non-zero the
 * service issues VIDIOC_S_FMT and adopts whatever values the driver returns.
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;  /* V4L2 FOURCC code; 0 keeps driver default */
    bool nearest;           /* True to try the closest supported size when VIDIOC_S_FMT rejects the requested size */
} camera_open_opts_t;

typedef struct {
    uint32_t pixel_format;
    char pixel_format_str[5];
    char description[CAMERA_FORMAT_DESCRIPTION_MAX];
    uint32_t flags;          /* raw V4L2_FMT_FLAG_* bitmask */
    bool compressed;         /* derived: V4L2_FMT_FLAG_COMPRESSED */
    bool emulated;           /* derived: V4L2_FMT_FLAG_EMULATED */
} camera_format_desc_t;

typedef enum {
    CAMERA_FRAME_SIZE_DISCRETE = 0,
    CAMERA_FRAME_SIZE_STEPWISE,
    CAMERA_FRAME_SIZE_CONTINUOUS,
} camera_frame_size_type_t;

typedef struct {
    camera_frame_size_type_t type;
    /* DISCRETE */
    uint32_t width;
    uint32_t height;
    /* STEPWISE / CONTINUOUS */
    uint32_t min_width;
    uint32_t max_width;
    uint32_t step_width;
    uint32_t min_height;
    uint32_t max_height;
    uint32_t step_height;
} camera_frame_size_t;

typedef struct {
    camera_frame_size_type_t type;
    /* DISCRETE */
    uint32_t numerator;
    uint32_t denominator;
    /* STEPWISE / CONTINUOUS */
    uint32_t min_numerator;
    uint32_t min_denominator;
    uint32_t max_numerator;
    uint32_t max_denominator;
    uint32_t step_numerator;
    uint32_t step_denominator;
} camera_frame_interval_t;

esp_err_t camera_open(const char *dev_path, const camera_open_opts_t *opts);
esp_err_t camera_get_stream_info(camera_stream_info_t *out_info);
esp_err_t camera_capture_frame(int timeout_ms, uint8_t **frame_data, size_t *frame_bytes,
                               camera_frame_info_t *out_info);
esp_err_t camera_release_frame(void *frame_data);
esp_err_t camera_close(void);

/**
 * @brief Drop every queued capture buffer so the next get_frame returns a fresh capture.
 *
 * Fails with ESP_ERR_INVALID_STATE when the camera is not opened or one or
 * more frames are still borrowed.
 */
esp_err_t camera_flush(void);

bool camera_is_open(void);
bool camera_is_streaming(void);

/**
 * @brief Enumerate supported pixel formats via VIDIOC_ENUM_FMT.
 *
 * @return ESP_OK when a format exists at @p index;
 *         ESP_ERR_NOT_FOUND when the index is past the end (loop terminator);
 *         ESP_ERR_INVALID_STATE when the camera is not opened.
 */
esp_err_t camera_enum_format(uint32_t index, camera_format_desc_t *out);

/**
 * @brief Enumerate frame sizes for one pixel format via VIDIOC_ENUM_FRAMESIZES.
 *
 * @return ESP_OK with type=DISCRETE for each discrete size; ESP_OK with
 *         type=STEPWISE/CONTINUOUS for a single range descriptor (driver
 *         returns one entry at index 0 in this case); ESP_ERR_NOT_FOUND when
 *         the index is past the end.
 */
esp_err_t camera_enum_frame_size(uint32_t pixel_format, uint32_t index,
                                 camera_frame_size_t *out);

/**
 * @brief Enumerate frame intervals (i.e. supported FPS) for a given format+size.
 *
 * Driver may report DISCRETE entries one by one, or a single STEPWISE /
 * CONTINUOUS range at index 0. fps = denominator / numerator.
 */
esp_err_t camera_enum_frame_interval(uint32_t pixel_format,
                                     uint32_t width, uint32_t height,
                                     uint32_t index,
                                     camera_frame_interval_t *out);

#ifdef __cplusplus
}
#endif
