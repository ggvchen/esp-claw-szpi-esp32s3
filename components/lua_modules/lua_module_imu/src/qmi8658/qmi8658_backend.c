/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658 backend for lua_module_imu. The implementation uses direct register
 * access through the i2c_bus device handle created by the generic IMU module.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.qmi8658";

#define QMI8658_I2C_ADDR_LOW        0x6a
#define QMI8658_I2C_ADDR_HIGH       0x6b
#define QMI8658_WHO_AM_I_VALUE      0x05

#define QMI8658_REG_WHO_AM_I        0x00
#define QMI8658_REG_CTRL1           0x02
#define QMI8658_REG_CTRL2           0x03
#define QMI8658_REG_CTRL3           0x04
#define QMI8658_REG_CTRL7           0x08
#define QMI8658_REG_STATUS0         0x2e
#define QMI8658_REG_TEMP_L          0x33
#define QMI8658_REG_AX_L            0x35

static int16_t qmi8658_le16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static esp_err_t qmi8658_read(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    if (ctx == NULL || ctx->i2c_dev_handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_read_bytes(ctx->i2c_dev_handle, reg, len, data);
}

static esp_err_t qmi8658_write_byte(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t value)
{
    if (ctx == NULL || ctx->i2c_dev_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_write_byte(ctx->i2c_dev_handle, reg, value);
}

static esp_err_t qmi8658_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    esp_err_t err = lua_imu_ctx_select_addr(ctx, i2c_addr);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t chip_id = 0;
    err = qmi8658_read(ctx, QMI8658_REG_WHO_AM_I, &chip_id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (chip_id != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "Unexpected QMI8658 WHO_AM_I 0x%02x at 0x%02x", chip_id, i2c_addr);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(qmi8658_write_byte(ctx, QMI8658_REG_CTRL1, 0x40),
                        TAG, "failed to configure address auto-increment");
    ESP_RETURN_ON_ERROR(qmi8658_write_byte(ctx, QMI8658_REG_CTRL7, 0x03),
                        TAG, "failed to enable accelerometer and gyroscope");
    ESP_RETURN_ON_ERROR(qmi8658_write_byte(ctx, QMI8658_REG_CTRL2, 0x95),
                        TAG, "failed to configure accelerometer");
    ESP_RETURN_ON_ERROR(qmi8658_write_byte(ctx, QMI8658_REG_CTRL3, 0xd5),
                        TAG, "failed to configure gyroscope");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "QMI8658 initialized at 0x%02x", i2c_addr);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    uint8_t status = 0;
    uint8_t raw[12] = { 0 };

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(qmi8658_read(ctx, QMI8658_REG_STATUS0, &status, 1),
                        TAG, "failed to read status");
    if ((status & 0x03) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(qmi8658_read(ctx, QMI8658_REG_AX_L, raw, sizeof(raw)),
                        TAG, "failed to read accel/gyro sample");

    out->accel.x = qmi8658_le16(&raw[0]);
    out->accel.y = qmi8658_le16(&raw[2]);
    out->accel.z = qmi8658_le16(&raw[4]);
    out->gyro.x = qmi8658_le16(&raw[6]);
    out->gyro.y = qmi8658_le16(&raw[8]);
    out->gyro.z = qmi8658_le16(&raw[10]);
    out->sens_time = esp_timer_get_time();
    out->status = status;
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    uint8_t raw[2] = { 0 };

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(qmi8658_read(ctx, QMI8658_REG_TEMP_L, raw, sizeof(raw)),
                        TAG, "failed to read temperature");
    *out = qmi8658_le16(raw);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    uint8_t status = 0;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(qmi8658_read(ctx, QMI8658_REG_STATUS0, &status, 1),
                        TAG, "failed to read status");
    *out = status;
    return ESP_OK;
}

static bool qmi8658_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == QMI8658_I2C_ADDR_LOW || i2c_addr == QMI8658_I2C_ADDR_HIGH;
}

static uint8_t qmi8658_backend_default_addr(void)
{
    return QMI8658_I2C_ADDR_LOW;
}

static int qmi8658_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    return (i2c_addr == QMI8658_I2C_ADDR_HIGH) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend = {
    .chip_name = "qmi8658",
    .state_size = 0,
    .probe = qmi8658_backend_probe,
    .destroy = NULL,
    .read_sample = qmi8658_backend_read_sample,
    .read_temperature = qmi8658_backend_read_temperature,
    .read_int_status = qmi8658_backend_read_int_status,
    .is_supported_addr = qmi8658_backend_is_supported_addr,
    .default_addr = qmi8658_backend_default_addr,
    .sdo_level_for_addr = qmi8658_backend_sdo_level_for_addr,
};
