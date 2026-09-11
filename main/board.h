#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize board hardware (IO expander, codec, touch, display).
void board_init(void);

/// Register a callback for knob button press.
void board_set_btn_press_cb(void (*cb)(void));

/// Register a callback for knob button release.
void board_set_btn_release_cb(void (*cb)(void));

/// Register a callback for knob button long-press release.
void board_set_btn_long_press_cb(void (*cb)(void));

/// Register a callback for a single click of the knob button: a press and
/// release that was not held long enough to be a long press. It arrives a
/// short click window after the release, once no second click can follow.
void board_set_btn_click_cb(void (*cb)(void));

/// Register a callback for knob rotation. `dir` is +1 clockwise, -1 anti.
/// Runs from the knob's own timer context, not the LVGL task.
void board_set_knob_cb(void (*cb)(int dir));

/// Enter deep sleep. Wakes on button press (or after time_sec seconds if > 0).
void board_deep_sleep(uint32_t time_sec);

/// Switch the SD card's power rail. Off at boot; the recorder raises it
/// before mounting and drops it again once the file is closed.
void board_sdcard_power(bool on);

/// Hold the audio codec powered. Counted: the codec, its I2S clocks and the
/// amplifier rail come up on the first acquire and go down again on the
/// release that matches it. Every acquire needs a release. After a resume
/// the output volume is back at the codec's default, so set it again.
void board_codec_acquire(void);
void board_codec_release(void);

/// Set LCD backlight brightness (0-100%).
void board_set_lcd_brightness(int percent);

#ifdef __cplusplus
}
#endif
