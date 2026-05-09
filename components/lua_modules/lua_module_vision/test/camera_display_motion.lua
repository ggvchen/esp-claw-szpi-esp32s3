local board_manager = require("board_manager")
local camera = require("camera")
local delay = require("delay")
local display = require("display")
local image = require("image")
local motion = require("motion_detect")

local TAG = "[camera_display_motion]"
local RUN_SECONDS = 30
local CAPTURE_TIMEOUT_MS = 3000
local FRAME_INTERVAL_MS = 30
local CAMERA_OPEN_OPTS = { format = "JPEG", width = 320, height = 240 }
local MOTION_OPTS = {
    stride = 8,
    pixel_threshold = 0.2,
    moving_threshold = 0.02,
}

local display_started = false
local camera_started = false

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

local function draw_result_overlay(frame_index, remaining_s, detect_result)
    local has_previous = detect_result.has_previous == true
    local moving_points = detect_result.moving_points or 0
    local moving_ratio = detect_result.moving_ratio or 0
    local moved = detect_result.moved == true
    local status = moved and "MOTION" or (has_previous and "STILL" or "WARMUP")
    local bg_r, bg_g, bg_b = 24, 24, 24

    if moved then
        bg_r, bg_g, bg_b = 160, 24, 24
    elseif has_previous then
        bg_r, bg_g, bg_b = 24, 96, 48
    end

    -- Draw an ASCII status bar after the camera preview so detection result is visible on screen.
    display.fill_rect(0, 0, display.width(), 48, bg_r, bg_g, bg_b)
    display.draw_text(8, 6, string.format("motion: %s", status), {
        r = 255,
        g = 255,
        b = 255,
        font_size = 16,
    })
    display.draw_text(8, 28, string.format("ratio=%.3f points=%d frame=%d left=%ds", moving_ratio, moving_points, frame_index, remaining_s), {
        r = 255,
        g = 255,
        b = 255,
        font_size = 12,
    })
end

local panel_handle, io_handle, lcd_width, lcd_height, panel_if = board_manager.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " SKIP: get_display_lcd_params failed: " .. tostring(io_handle))
    return
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " SKIP: get_camera_paths failed: " .. tostring(path_err))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, lcd_width, lcd_height, panel_if)
if not ok then
    print(TAG .. " SKIP: display.init failed: " .. tostring(err))
    return
end
display_started = true

ok, err = pcall(camera.open, camera_paths.dev_path, CAMERA_OPEN_OPTS)
if not ok then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(err))
    cleanup()
    return
end
camera_started = true

local run_ok, run_err = xpcall(function()
    local stream = camera.info()
    local start_s = os.time()
    local deadline_s = start_s + RUN_SECONDS
    local frames = 0
    local moved_frames = 0

    print(string.format("%s start %ds stream=%dx%d format=%s",
        TAG, RUN_SECONDS, stream.width, stream.height, tostring(stream.pixel_format)))

    while os.time() < deadline_s do
        local now_s = os.time()
        local remaining_s = deadline_s - now_s
        local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
        local rgb565 <close> = image.convert(frame, image.RGB565)
        local rgb_info = rgb565:info()
        local detect_result = motion.detect(frame, MOTION_OPTS)

        frames = frames + 1
        if detect_result.moved == true then
            moved_frames = moved_frames + 1
        end

        display.begin_frame({ clear = true, r = 0, g = 0, b = 0 })
        display.draw_rgb565_fit(0, 0, rgb_info.width, rgb_info.height, display.width(), display.height(), rgb565:data())
        draw_result_overlay(frames, remaining_s, detect_result)
        display.present()
        display.end_frame()

        if frames == 1 or frames % 15 == 0 then
            print(string.format("%s frame=%d moved=%s points=%s ratio=%s moved_frames=%d",
                TAG, frames, tostring(detect_result.moved), tostring(detect_result.moving_points), tostring(detect_result.moving_ratio), moved_frames))
        end

        if FRAME_INTERVAL_MS > 0 then
            delay.delay_ms(FRAME_INTERVAL_MS)
        end
    end

    display.begin_frame({ clear = true, r = 0, g = 0, b = 0 })
    display.draw_text_aligned(0, 0, display.width(), display.height(), string.format("Motion test done\nframes=%d moved=%d", frames, moved_frames), {
        r = 255,
        g = 255,
        b = 255,
        font_size = 20,
        align = "center",
        valign = "middle",
    })
    display.present()
    display.end_frame()

    print(string.format("%s PASS frames=%d moved_frames=%d", TAG, frames, moved_frames))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
