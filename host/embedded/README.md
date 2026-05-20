# Cronopio embedded host

Placeholder. The embedded target reuses `host/common/` (which has no
OS dependencies) and plugs in a small HAL:

| HAL function          | Responsibility                                    |
|-----------------------|---------------------------------------------------|
| `hal_init()`          | clocks, peripherals, framebuffer driver           |
| `hal_present(rgba)`   | DMA the 320×240 RGB565/RGB888 buffer to the panel |
| `hal_audio_push(buf)` | feed the I²S/PWM ring with mixed audio            |
| `hal_input_poll()`    | return gamepad bitmask                            |
| `hal_time_ms()`       | monotonic milliseconds                            |
| `hal_save_load/save`  | NVS or external flash                             |

Target boards considered (no commitment yet): RP2040 + ST7789,
ESP32-S3 + I2S DAC, STM32H7 with parallel LCD.
