/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "emote.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "display_session.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_mmap_assets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "lvgl.h"

static const char *TAG = "app_emote";

#define EMOTE_STATUS_MAX 96
#define EMOTE_ASSETS_PARTITION "emote"
#define EMOTE_ANIM_ONLINE "swim.eaf"
#define EMOTE_ANIM_OFFLINE "offline.eaf"
#define EMOTE_ANIM_Y_OFFSET 20
#define EMOTE_TITLE_Y 4
#define EMOTE_TITLE_H 28
#define EMOTE_FALLBACK_H 72

static display_session_t *s_display_session;
static mmap_assets_handle_t s_assets_handle;
static gfx_obj_t *s_anim_obj;
static gfx_obj_t *s_title_label;
static gfx_obj_t *s_fallback_label;
static void *s_anim_data;
static size_t s_anim_data_len;
static char s_status_text[EMOTE_STATUS_MAX] = "Wi-Fi offline";
static bool s_sta_connected;
static bool s_started;
static TaskHandle_t s_direct_test_task;

static gfx_color_t emote_color(gfx_color_t color)
{
    if (display_session_should_swap_color(s_display_session)) {
        color.full = (uint16_t)((color.full << 8) | (color.full >> 8));
    }
    return color;
}

static int emote_find_asset_id_by_name(const char *filename)
{
    if (s_assets_handle == NULL || filename == NULL) {
        return -1;
    }

    int files = mmap_assets_get_stored_files(s_assets_handle);
    for (int i = 0; i < files; i++) {
        const char *name = mmap_assets_get_name(s_assets_handle, i);
        if (name != NULL && strcmp(name, filename) == 0) {
            return i;
        }
    }

    return -1;
}

static esp_err_t emote_mount_assets(void)
{
    if (s_assets_handle != NULL) {
        return ESP_OK;
    }

    const mmap_assets_config_t asset_config = {
        .partition_label = EMOTE_ASSETS_PARTITION,
        .flags = {
            .mmap_enable = true,
            .use_fs = false,
            .app_bin_check = false,
        },
    };

    return mmap_assets_new(&asset_config, &s_assets_handle);
}

static esp_err_t emote_set_anim_locked(const char *filename)
{
    ESP_RETURN_ON_FALSE(s_anim_obj != NULL && s_assets_handle != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "animation is not ready");

    int asset_id = emote_find_asset_id_by_name(filename);
    ESP_RETURN_ON_FALSE(asset_id >= 0, ESP_ERR_NOT_FOUND, TAG, "animation asset not found: %s", filename);

    const void *anim_data = mmap_assets_get_mem(s_assets_handle, asset_id);
    int anim_size = mmap_assets_get_size(s_assets_handle, asset_id);
    ESP_RETURN_ON_FALSE(anim_data != NULL && anim_size > 0,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid animation asset: %s", filename);

    void *copied_data = heap_caps_malloc((size_t)anim_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copied_data == NULL) {
        copied_data = heap_caps_malloc((size_t)anim_size, MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(copied_data != NULL, ESP_ERR_NO_MEM, TAG, "allocate animation asset failed: %s", filename);

    size_t copied_size = mmap_assets_copy_mem(s_assets_handle, (size_t)anim_data, copied_data, (size_t)anim_size);
    if (copied_size != (size_t)anim_size) {
        free(copied_data);
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG,
                            "copy animation asset failed: %s copied=%u expected=%u",
                            filename,
                            (unsigned int)copied_size,
                            (unsigned int)anim_size);
    }

    const gfx_anim_src_t anim_src = {
        .type = GFX_ANIM_SRC_TYPE_MEMORY,
        .data = copied_data,
        .data_len = copied_size,
    };

    (void)gfx_anim_stop(s_anim_obj);
    esp_err_t ret = gfx_anim_set_src_desc(s_anim_obj, &anim_src);
    if (ret != ESP_OK) {
        free(copied_data);
        ESP_RETURN_ON_ERROR(ret, TAG, "set animation source failed");
    }

    free(s_anim_data);
    s_anim_data = copied_data;
    s_anim_data_len = copied_size;

    ESP_RETURN_ON_ERROR(gfx_obj_align(s_anim_obj, GFX_ALIGN_CENTER, 0, EMOTE_ANIM_Y_OFFSET),
                        TAG, "align animation failed");
    ESP_RETURN_ON_ERROR(gfx_anim_set_segment(s_anim_obj, 0, 0xFFFFFFFF, 20, true),
                        TAG, "set animation segment failed");
    return gfx_anim_start(s_anim_obj);
}

static void emote_format_network_status(bool sta_connected, const char *ap_ssid)
{
    const bool ap_present = (ap_ssid != NULL && ap_ssid[0] != '\0');

    if (sta_connected && ap_present) {
        snprintf(s_status_text, sizeof(s_status_text), "Online * AP: %s", ap_ssid);
    } else if (sta_connected) {
        strlcpy(s_status_text, "Wi-Fi connected", sizeof(s_status_text));
    } else if (ap_present) {
        snprintf(s_status_text, sizeof(s_status_text), "Setup WiFi: %s", ap_ssid);
    } else {
        strlcpy(s_status_text, "Wi-Fi offline", sizeof(s_status_text));
    }
}

static esp_err_t emote_create_title_label_locked(gfx_disp_t *disp)
{
    uint32_t screen_w;

    ESP_RETURN_ON_FALSE(disp != NULL && s_display_session != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid title label args");

    screen_w = display_session_width(s_display_session);
    s_title_label = gfx_label_create(disp);
    ESP_RETURN_ON_FALSE(s_title_label != NULL, ESP_ERR_NO_MEM, TAG, "create title label failed");

    (void)gfx_obj_set_pos(s_title_label, 0, EMOTE_TITLE_Y);
    (void)gfx_obj_set_size(s_title_label, (gfx_coord_t)screen_w, EMOTE_TITLE_H);
    (void)gfx_label_set_font(s_title_label, (gfx_font_t)LV_FONT_DEFAULT);
    (void)gfx_label_set_color(s_title_label, emote_color(GFX_COLOR_HEX(0xFFFFFF)));
    (void)gfx_label_set_bg_enable(s_title_label, false);
    (void)gfx_label_set_text_align(s_title_label, GFX_TEXT_ALIGN_CENTER);
    (void)gfx_label_set_long_mode(s_title_label, GFX_LABEL_LONG_SCROLL);
    (void)gfx_label_set_text(s_title_label, s_status_text);
    return ESP_OK;
}

static esp_err_t emote_create_fallback_label_locked(gfx_disp_t *disp)
{
    uint32_t screen_w;
    char text[EMOTE_STATUS_MAX + 16];

    ESP_RETURN_ON_FALSE(disp != NULL && s_display_session != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fallback label args");

    screen_w = display_session_width(s_display_session);
    s_fallback_label = gfx_label_create(disp);
    ESP_RETURN_ON_FALSE(s_fallback_label != NULL, ESP_ERR_NO_MEM, TAG, "create fallback label failed");

    (void)gfx_obj_set_size(s_fallback_label, (gfx_coord_t)screen_w, EMOTE_FALLBACK_H);
    (void)gfx_obj_align(s_fallback_label, GFX_ALIGN_CENTER, 0, 0);
    (void)gfx_label_set_font(s_fallback_label, (gfx_font_t)LV_FONT_DEFAULT);
    (void)gfx_label_set_color(s_fallback_label, emote_color(GFX_COLOR_HEX(0xFFFFFF)));
    (void)gfx_label_set_bg_enable(s_fallback_label, false);
    (void)gfx_label_set_text_align(s_fallback_label, GFX_TEXT_ALIGN_CENTER);
    (void)gfx_label_set_long_mode(s_fallback_label, GFX_LABEL_LONG_SCROLL);
    snprintf(text, sizeof(text), "ESP-Claw | %s", s_status_text);
    (void)gfx_label_set_text(s_fallback_label, text);
    return ESP_OK;
}

static void emote_update_fallback_label_locked(void)
{
    char text[EMOTE_STATUS_MAX + 16];

    if (s_fallback_label == NULL) {
        return;
    }
    snprintf(text, sizeof(text), "ESP-Claw | %s", s_status_text);
    (void)gfx_label_set_text(s_fallback_label, text);
}

static void emote_draw_direct_panel_test_locked(void)
{
    esp_lcd_panel_handle_t panel;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t band_h;
    uint16_t *band;

    if (s_display_session == NULL) {
        return;
    }

    panel = display_session_panel(s_display_session);
    screen_w = display_session_width(s_display_session);
    screen_h = display_session_height(s_display_session);
    if (panel == NULL || screen_w == 0 || screen_h == 0) {
        return;
    }

    band_h = screen_h / 4;
    if (band_h == 0) {
        band_h = 1;
    }
    band = heap_caps_malloc(screen_w * band_h * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (band == NULL) {
        band = heap_caps_malloc(screen_w * band_h * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (band == NULL) {
        ESP_LOGW(TAG, "direct panel test buffer allocation failed");
        return;
    }

    const uint16_t colors[] = {
        0xF800,
        0x07E0,
        0x001F,
        0xFFFF,
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        for (uint32_t p = 0; p < screen_w * band_h; ++p) {
            band[p] = colors[i];
        }
        uint32_t y0 = i * band_h;
        uint32_t y1 = (i == 3) ? screen_h : (y0 + band_h);
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, 0, (int)y0, (int)screen_w, (int)y1, band);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "direct panel test draw failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    free(band);
    ESP_LOGW(TAG, "direct panel test drawn: %ux%u", (unsigned)screen_w, (unsigned)screen_h);
}

static void emote_direct_test_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < 30; ++i) {
        if (s_display_session != NULL && display_session_lock(s_display_session) == ESP_OK) {
            emote_draw_direct_panel_test_locked();
            display_session_unlock(s_display_session);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    s_direct_test_task = NULL;
    vTaskDelete(NULL);
}

static void emote_start_direct_test_task(void)
{
    if (s_direct_test_task != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(emote_direct_test_task,
                                "lcd_direct_test",
                                3072,
                                NULL,
                                3,
                                &s_direct_test_task);
    if (ok != pdPASS) {
        s_direct_test_task = NULL;
        ESP_LOGW(TAG, "start direct panel test task failed");
    }
}

static void emote_delete_ui_locked(void)
{
    if (s_anim_obj != NULL) {
        (void)gfx_anim_stop(s_anim_obj);
        (void)gfx_obj_delete(s_anim_obj);
        s_anim_obj = NULL;
    }
    free(s_anim_data);
    s_anim_data = NULL;
    s_anim_data_len = 0;
    if (s_title_label != NULL) {
        (void)gfx_obj_delete(s_title_label);
        s_title_label = NULL;
    }
    if (s_fallback_label != NULL) {
        (void)gfx_obj_delete(s_fallback_label);
        s_fallback_label = NULL;
    }
}

static esp_err_t emote_create_ui(void)
{
    gfx_disp_t *disp;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(s_display_session != NULL, ESP_ERR_INVALID_STATE, TAG, "display session not started");
    ESP_RETURN_ON_ERROR(display_session_lock(s_display_session), TAG, "lock display failed");

    disp = display_session_display(s_display_session);

    emote_delete_ui_locked();
    (void)gfx_disp_set_bg_color(disp, GFX_COLOR_HEX(0x171617));

    s_anim_obj = gfx_anim_create(disp);
    ESP_GOTO_ON_FALSE(s_anim_obj != NULL, ESP_ERR_NO_MEM, err, TAG, "create animation failed");
    (void)gfx_anim_set_auto_mirror(s_anim_obj, false);
    ret = emote_set_anim_locked(s_sta_connected ? EMOTE_ANIM_ONLINE : EMOTE_ANIM_OFFLINE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "animation disabled, fallback text UI active: %s", esp_err_to_name(ret));
        (void)gfx_obj_delete(s_anim_obj);
        s_anim_obj = NULL;
    }
    ret = emote_create_title_label_locked(disp);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "create title label failed");
    if (s_anim_obj == NULL) {
        ret = emote_create_fallback_label_locked(disp);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "create fallback label failed");
    }

    gfx_disp_refresh_all(disp);
    emote_draw_direct_panel_test_locked();
    display_session_unlock(s_display_session);
    return ESP_OK;

err:
    emote_delete_ui_locked();
    display_session_unlock(s_display_session);
    return ret;
}

esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)
{
    bool status_changed = s_sta_connected != sta_connected;

    s_sta_connected = sta_connected;
    emote_format_network_status(sta_connected, ap_ssid);

    if (!s_started || s_display_session == NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_session_lock(s_display_session), TAG, "lock display failed");
    if (status_changed && s_anim_obj != NULL && s_assets_handle != NULL) {
        esp_err_t ret = emote_set_anim_locked(s_sta_connected ? EMOTE_ANIM_ONLINE : EMOTE_ANIM_OFFLINE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "switch animation failed: %s", esp_err_to_name(ret));
            (void)gfx_obj_delete(s_anim_obj);
            s_anim_obj = NULL;
            free(s_anim_data);
            s_anim_data = NULL;
            s_anim_data_len = 0;
            if (s_fallback_label == NULL) {
                (void)emote_create_fallback_label_locked(display_session_display(s_display_session));
            }
            emote_draw_direct_panel_test_locked();
        }
    }
    if (s_title_label != NULL) {
        (void)gfx_label_set_text(s_title_label, s_status_text);
    }
    emote_update_fallback_label_locked();
    gfx_disp_refresh_all(display_session_display(s_display_session));
    display_session_unlock(s_display_session);
    return ESP_OK;
}

esp_err_t emote_start(void)
{
    esp_err_t ret;

    if (s_started) {
        return ESP_OK;
    }

    const display_session_config_t session_config = {0};
    ESP_RETURN_ON_ERROR(display_session_start(&s_display_session, &session_config),
                        TAG, "start emote display session failed");

    ret = emote_mount_assets();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount emote animation assets failed: %s", esp_err_to_name(ret));
        display_session_stop(&s_display_session);
        return ret;
    }

    s_started = true;
    ret = emote_create_ui();
    if (ret != ESP_OK) {
        s_started = false;
        if (s_assets_handle != NULL) {
            (void)mmap_assets_del(s_assets_handle);
            s_assets_handle = NULL;
        }
        display_session_stop(&s_display_session);
        return ret;
    }
    emote_start_direct_test_task();
    return ESP_OK;
}
