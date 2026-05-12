/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_camera.h"

#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "lua_image.h"
#include "lauxlib.h"
#include "lua.h"
#include "camera_hal.h"

#define LUA_MODULE_CAMERA_NAME "camera"

/* Camera-side release hook. The service can hold multiple borrowed buffers,
 * each identified by its data pointer; ctx is unused. */
static void lua_module_camera_frame_release_cb(void *ctx, const uint8_t *data)
{
    (void)ctx;
    if (data != NULL) {
        camera_release_frame((void *)data);
    }
}

/* Pack a 4-char FOURCC string into the V4L2 uint32 code. Rejects anything that
 * is not exactly four printable bytes. */
static int lua_module_camera_parse_fourcc(lua_State *L, const char *fourcc, uint32_t *out)
{
    size_t len = (fourcc != NULL) ? strlen(fourcc) : 0;
    if (len != 4) {
        return luaL_error(L, "pixel format must be a 4-char FOURCC string (got %d chars)", (int)len);
    }
    *out = ((uint32_t)(uint8_t)fourcc[0])
         | ((uint32_t)(uint8_t)fourcc[1] << 8)
         | ((uint32_t)(uint8_t)fourcc[2] << 16)
         | ((uint32_t)(uint8_t)fourcc[3] << 24);
    return 0;
}

static int lua_module_camera_optional_uint_field(lua_State *L, int table_idx, const char *name, uint32_t *out)
{
    lua_getfield(L, table_idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_isinteger(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "opts.%s must be an integer", name);
    }
    lua_Integer value = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (value < 0 || value > UINT32_MAX) {
        return luaL_error(L, "opts.%s out of range", name);
    }
    *out = (uint32_t)value;
    return 0;
}

static int lua_module_camera_optional_bool_field(lua_State *L, int table_idx, const char *name, bool *out)
{
    lua_getfield(L, table_idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_isboolean(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "opts.%s must be a boolean", name);
    }
    *out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return 0;
}

/* camera.open(dev_path [, opts])
 * opts (optional table): { width, height, format, nearest }
 *   - width/height: requested capture resolution (integer; driver may adjust)
 *   - format: FOURCC string e.g. "RGBP", "RGBR", "YUYV", "JPEG", "MJPG"
 *   - nearest: try the closest supported size when the exact size is rejected
 * If any opts field is non-nil the driver is asked via VIDIOC_S_FMT; the
 * actually-negotiated values are reflected in camera.info(). */
static int lua_module_camera_open(lua_State *L)
{
    const char *dev_path = luaL_checkstring(L, 1);
    camera_open_opts_t opts = {0};
    const camera_open_opts_t *opts_ptr = NULL;
    esp_err_t err;

    if (!lua_isnoneornil(L, 2)) {
        if (!lua_istable(L, 2)) {
            return luaL_error(L, "camera.open opts must be a table");
        }
        lua_module_camera_optional_uint_field(L, 2, "width", &opts.width);
        lua_module_camera_optional_uint_field(L, 2, "height", &opts.height);
        lua_module_camera_optional_bool_field(L, 2, "nearest", &opts.nearest);
        lua_getfield(L, 2, "format");
        if (!lua_isnil(L, -1)) {
            const char *fourcc = luaL_checkstring(L, -1);
            lua_module_camera_parse_fourcc(L, fourcc, &opts.pixel_format);
        }
        lua_pop(L, 1);
        opts_ptr = &opts;
    }

    err = camera_open(dev_path, opts_ptr);
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera open with opts requires close first (already open)");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera open failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* camera.is_open() -> bool */
static int lua_module_camera_is_open(lua_State *L)
{
    lua_pushboolean(L, camera_is_open());
    return 1;
}

/* camera.is_streaming() -> bool */
static int lua_module_camera_is_streaming(lua_State *L)
{
    lua_pushboolean(L, camera_is_streaming());
    return 1;
}

/* camera.flush()
 * Drops every queued buffer so the next get_frame() returns a freshly captured
 * frame. Useful after long idle / wake-up to discard stale AE/AWB output. */
static int lua_module_camera_flush(lua_State *L)
{
    esp_err_t err = camera_flush();

    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera flush failed: not streaming or one or more frames are still borrowed");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera flush failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Push a Lua array of discrete fps integers onto the stack. Stepwise / range
 * intervals are intentionally ignored — they are rare and surfacing them
 * doubles the output shape. Returns the number of fps entries pushed; caller
 * decides whether to attach (count > 0) or discard (count == 0). */
static int lua_module_camera_push_fps_for_size(lua_State *L, uint32_t pixel_format,
                                               uint32_t w, uint32_t h)
{
    camera_frame_interval_t interval = {0};
    int count = 0;

    lua_newtable(L); /* fps array */
    for (uint32_t k = 0; ; k++) {
        esp_err_t err = camera_enum_frame_interval(pixel_format, w, h, k, &interval);
        if (err != ESP_OK) {
            break;
        }
        if (interval.type != CAMERA_FRAME_SIZE_DISCRETE) {
            break;
        }
        if (interval.numerator == 0) {
            continue;
        }
        if (interval.denominator % interval.numerator == 0) {
            lua_pushinteger(L, (lua_Integer)(interval.denominator / interval.numerator));
        } else {
            lua_pushnumber(L, (lua_Number)interval.denominator / (lua_Number)interval.numerator);
        }
        lua_rawseti(L, -2, ++count);
    }
    return count;
}

/* camera.list_formats() — see README. Output shape:
 *   {
 *     { format = "JPEG", description = "...",
 *       sizes = { { w=640, h=480, fps = {30, 15} }, ... },
 *     },
 *     ...
 *   }
 * Empty fps arrays, stepwise/continuous size ranges, and verbose driver flags
 * are intentionally omitted; query the V4L2 raw enum APIs via custom C code if
 * those are needed. */
static int lua_module_camera_list_formats(lua_State *L)
{
    camera_format_desc_t desc = {0};

    lua_newtable(L);

    for (uint32_t i = 0; ; i++) {
        esp_err_t err = camera_enum_format(i, &desc);
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return luaL_error(L, "camera list_formats failed: camera not opened");
        }
        if (err != ESP_OK) {
            return luaL_error(L, "camera list_formats failed: %s", esp_err_to_name(err));
        }

        lua_newtable(L); /* format entry */
        lua_pushstring(L, desc.pixel_format_str);
        lua_setfield(L, -2, "format");
        lua_pushstring(L, desc.description);
        lua_setfield(L, -2, "description");

        lua_newtable(L); /* sizes (array of discrete) */
        int discrete_count = 0;

        for (uint32_t j = 0; ; j++) {
            camera_frame_size_t fsz = {0};
            esp_err_t serr = camera_enum_frame_size(desc.pixel_format, j, &fsz);
            if (serr == ESP_ERR_NOT_FOUND) {
                break;
            }
            if (serr != ESP_OK) {
                return luaL_error(L, "camera enum_frame_size failed: %s", esp_err_to_name(serr));
            }

            if (fsz.type != CAMERA_FRAME_SIZE_DISCRETE) {
                /* Keep the Lua shape simple: list_formats exposes only discrete sizes. */
                break;
            }

            lua_newtable(L); /* size entry */
            lua_pushinteger(L, (lua_Integer)fsz.width);
            lua_setfield(L, -2, "w");
            lua_pushinteger(L, (lua_Integer)fsz.height);
            lua_setfield(L, -2, "h");
            /* Only attach fps when the driver enumerates discrete intervals. */
            int fps_count = lua_module_camera_push_fps_for_size(L, desc.pixel_format,
                                                                fsz.width, fsz.height);
            if (fps_count > 0) {
                lua_setfield(L, -2, "fps");
            } else {
                lua_pop(L, 1);
            }

            lua_rawseti(L, -2, ++discrete_count);
        }

        lua_setfield(L, -2, "sizes");

        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    return 1;
}

/* camera.info()
 * Returns a table with stream info: width, height, pixel_format. */
static int lua_module_camera_info(lua_State *L)
{
    camera_stream_info_t info = {0};
    esp_err_t err = camera_get_stream_info(&info);

    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera info failed: camera not opened, call camera.open() first");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera info failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushinteger(L, info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height);
    lua_setfield(L, -2, "height");
    lua_pushstring(L, info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    return 1;
}

/* camera.get_frame([timeout_ms])
 * Borrows one V4L2 buffer and wraps it as an image.frame userdata.
 * Release with frame:release() or by declaring the variable <close>. */
static int lua_module_camera_get_frame(lua_State *L)
{
    camera_frame_info_t frame_info = {0};
    lua_image_frame_info_t info = {0};
    int timeout_ms = (int)luaL_optinteger(L, 1, 0);
    uint8_t *frame_data = NULL;
    size_t frame_bytes = 0;
    esp_err_t err;

    if (timeout_ms < 0) {
        return luaL_error(L, "timeout_ms must be non-negative");
    }

    err = camera_capture_frame(timeout_ms, &frame_data, &frame_bytes, &frame_info);
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera get_frame failed: camera not opened or no capture buffer is available");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera get_frame failed: %s", esp_err_to_name(err));
    }

    info.width = (int)frame_info.width;
    info.height = (int)frame_info.height;
    info.bytes = frame_bytes;
    info.timestamp_us = frame_info.timestamp_us;
    strlcpy(info.pixel_format, frame_info.pixel_format_str, sizeof(info.pixel_format));

    err = lua_image_push_frame(L, frame_data, frame_bytes, &info,
                                    lua_module_camera_frame_release_cb, NULL);
    if (err != ESP_OK) {
        /* push failed before transferring ownership; we still hold the buffer */
        camera_release_frame(frame_data);
        return luaL_error(L, "camera get_frame failed: %s", esp_err_to_name(err));
    }
    return 1;
}

/* camera.close()
 * Closes the camera device and releases all resources. */
static int lua_module_camera_close(lua_State *L)
{
    esp_err_t err = camera_close();

    if (err != ESP_OK) {
        return luaL_error(L, "camera close failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_camera(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"open",         lua_module_camera_open},
        {"info",         lua_module_camera_info},
        {"get_frame",    lua_module_camera_get_frame},
        {"flush",        lua_module_camera_flush},
        {"is_open",      lua_module_camera_is_open},
        {"is_streaming", lua_module_camera_is_streaming},
        {"list_formats", lua_module_camera_list_formats},
        {"close",        lua_module_camera_close},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_camera_register(void)
{
    return cap_lua_register_module(LUA_MODULE_CAMERA_NAME, luaopen_camera);
}
