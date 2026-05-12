local board_manager = require("board_manager")
local camera = require("camera")
local delay = require("delay")
local display = require("display")
local image = require("image")

local TAG = "[camera_preview]"
local FRAME_TIMEOUT_MS = 3000
local FRAME_INTERVAL_MS = 30
local PREVIEW_FRAME_COUNT = 300 -- Set to 0 for continuous preview.

local camera_started = false
local display_started = false

local function cleanup()
    if display_started then
        pcall(display.end_frame)
        pcall(display.deinit)
        display_started = false
    end
    if camera_started then
        pcall(camera.close)
        camera_started = false
    end
end

local function panel_if_name(panel_if)
    if panel_if == board_manager.PANEL_IF_MIPI_DSI then
        return "mipi_dsi"
    end
    if panel_if == board_manager.PANEL_IF_RGB then
        return "rgb"
    end
    return "io"
end

local function draw_preview_frame(frame, lcd_w, lcd_h)
    -- Keep display format handling outside display: convert once, then draw RGB565 data.
    local rgb565 <close> = image.convert(frame, image.RGB565)
    local info = rgb565:info()
    display.begin_frame({ clear = true, r = 0, g = 0, b = 0 })
    local draw_w, draw_h = display.draw_rgb565_fit(0, 0, info.width, info.height, lcd_w, lcd_h, rgb565:data())
    display.present()
    display.end_frame()
    return draw_w, draw_h
end

local panel_handle, io_handle, lcd_width, lcd_height, panel_if = board_manager.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " ERROR: get_display_lcd_params failed: " .. tostring(io_handle))
    return
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " ERROR: get_camera_paths failed: " .. tostring(path_err))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, lcd_width, lcd_height, panel_if)
if not ok then
    print(TAG .. " ERROR: display.init failed: " .. tostring(err))
    return
end
display_started = true

open_format = { format = "JPEG", width = 320, height = 240}
ok, err = pcall(camera.open, camera_paths.dev_path, open_format)
if not ok then
    print(TAG .. " ERROR: camera.open failed: " .. tostring(err))
    cleanup()
    return
end
camera_started = true

local run_ok, run_err = xpcall(function()
    local stream = camera.info()
    local lcd_w = display.width()
    local lcd_h = display.height()
    local frames = 0

    print(string.format("%s start camera=%dx%d format=%s lcd=%dx%d panel_if=%s",
        TAG, stream.width, stream.height, tostring(stream.pixel_format), lcd_w, lcd_h, panel_if_name(panel_if)))

    while PREVIEW_FRAME_COUNT == 0 or frames < PREVIEW_FRAME_COUNT do
        local frame <close> = camera.get_frame(FRAME_TIMEOUT_MS)
        local draw_w, draw_h = draw_preview_frame(frame, lcd_w, lcd_h)
        frames = frames + 1

        if frames == 1 or frames % 30 == 0 then
            local info = frame:info()
            print(string.format("%s frame=%d source=%dx%d %s bytes=%d drawn=%dx%d",
                TAG, frames, info.width, info.height, tostring(info.pixel_format), info.bytes, draw_w, draw_h))
        end

        if FRAME_INTERVAL_MS > 0 then
            delay.delay_ms(FRAME_INTERVAL_MS)
        end
    end

    print(string.format("%s stopped after %d frame(s)", TAG, frames))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
