/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCKFB_SZPI_S3";

#define LCKFB_I2C_PORT              I2C_NUM_0
#define LCKFB_I2C_SDA_IO            1
#define LCKFB_I2C_SCL_IO            2
#define LCKFB_I2C_FREQ_HZ           100000

#define PCA9557_I2C_ADDR            0x18
#define PCA9557_OUTPUT_PORT         0x01
#define PCA9557_CONFIGURATION_PORT  0x03
#define PCA9557_LCD_CS_BIT          BIT(0)
#define PCA9557_PA_EN_BIT           BIT(1)
#define PCA9557_DVP_PWDN_BIT        BIT(2)

static esp_err_t ensure_i2c_driver(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = LCKFB_I2C_SDA_IO,
        .scl_io_num = LCKFB_I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = LCKFB_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(LCKFB_I2C_PORT, &i2c_conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = i2c_driver_install(LCKFB_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

static esp_err_t pca9557_write_reg(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(LCKFB_I2C_PORT, PCA9557_I2C_ADDR,
                                      write_buf, sizeof(write_buf),
                                      pdMS_TO_TICKS(1000));
}

static esp_err_t pca9557_prepare_board(void)
{
    static bool prepared;

    if (prepared) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_i2c_driver(), TAG, "failed to prepare I2C");

    /*
     * LCKFB SZPI uses PCA9557 IO0/IO1/IO2 for LCD_CS, PA_EN and DVP_PWDN.
     * Select the LCD, enable speaker PA, and release GC0308 from power-down
     * because LCD_CS and DVP_PWDN are not connected to native ESP32-S3 GPIOs.
     */
    const uint8_t output_value = PCA9557_PA_EN_BIT;
    ESP_RETURN_ON_ERROR(pca9557_write_reg(PCA9557_OUTPUT_PORT, output_value),
                        TAG, "failed to set PCA9557 outputs");
    ESP_RETURN_ON_ERROR(pca9557_write_reg(PCA9557_CONFIGURATION_PORT, 0xf8),
                        TAG, "failed to configure PCA9557 directions");

    prepared = true;
    ESP_LOGI(TAG, "PCA9557 prepared: LCD_CS=0, PA_EN=1, DVP_PWDN=0");
    return ESP_OK;
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_ERROR(pca9557_prepare_board(), TAG, "failed to prepare board expander");
    return esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_ERROR(pca9557_prepare_board(), TAG, "failed to prepare board expander");
    return esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
}
