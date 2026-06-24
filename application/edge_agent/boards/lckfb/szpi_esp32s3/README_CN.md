# 立创·实战派 ESP32-S3 适配说明

该目录为 `esp-claw` 的立创·实战派 ESP32-S3 板级配置，板卡 ID 为 `szpi_esp32s3`。

## 已适配

- 主控：`ESP32-S3-WROOM-1-N16R8`
- Flash：16 MB
- PSRAM：8 MB Octal PSRAM
- I2C：`SDA=GPIO1`，`SCL=GPIO2`
- 音频输入：`ES7210`
- 音频输出：`ES8311`
- 音频 I2S：`MCLK=GPIO38`，`BCLK=GPIO14`，`WS=GPIO13`，`DIN=GPIO12`
- LCD：`ST7789`，`320x240`，SPI 接口
- LCD 背光：`GPIO42`，低电平点亮，使用 LEDC 反相输出
- TF 卡：1-bit SDMMC，`CMD=GPIO48`，`CLK=GPIO47`，`D0=GPIO21`
- IO 扩展：`PCA9557`，用于 `LCD_CS`、`PA_EN`、`DVP_PWDN`
- 摄像头：`GC0308`，DVP 8-bit
- 电容触摸屏：`FT6336/FT6x36`，I2C 地址 `0x38`
- 姿态传感器：`QMI8658`，I2C 地址 `0x6A`

## 构建

在 `application/edge_agent` 目录执行：

```bash
idf.py set-target esp32s3
idf.py bmgr -c ./boards -b szpi_esp32s3
idf.py build
idf.py flash monitor
```

## 需要核对的引脚

公开教程和原理图截图明确给出了 I2C、音频 I2S、LCD、触摸、摄像头、QMI8658 和 TF 卡引脚。本适配中的 LCD SPI 引脚为：

- `SCLK=GPIO40`
- `MOSI=GPIO41`
- `DC=GPIO39`
- `CS=PCA9557 IO0`
- `RST=-1`

摄像头 DVP 引脚为：

- `D0=GPIO16`
- `D1=GPIO18`
- `D2=GPIO8`
- `D3=GPIO17`
- `D4=GPIO15`
- `D5=GPIO6`
- `D6=GPIO4`
- `D7=GPIO9`
- `PCLK=GPIO7`
- `VSYNC=GPIO3`
- `HREF=GPIO46`
- `XCLK=GPIO5`
- `PWDN=PCA9557 IO2`

如果你的板卡批次或原理图显示引脚不同，请修改 `board_peripherals.yaml`、`board_devices.yaml` 和 `setup_device.c`。
