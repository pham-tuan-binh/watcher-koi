#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Hard limits on how much the pond will simulate at once.
#define POND_MAX_KOI   12
#define POND_MAX_PADS  12
#define POND_MAX_MOTES 16

/// Create the koi pond on `parent` and start animating it.
void pond_init(lv_obj_t *parent);

/// Drop a ripple at a screen coordinate. The koi swim over to investigate.
void pond_tap(lv_coord_t x, lv_coord_t y);

/// Step the zoom by `delta` detents (positive zooms in). Zoomed in, the view
/// follows a koi. Returns true if the zoom actually changed.
bool pond_zoom(int delta);

/// Change how much is simulated. Counts are clamped to the POND_MAX_* limits
/// above; new koi, pads and motes are dropped into the pond and surplus ones
/// are removed. This is how many exist, not how many you can see. They roam
/// past the rim into the dark, so the number on screen drifts either side.
void pond_set_population(int koi, int pads, int motes);

/// Read back what is currently being simulated. Any pointer may be NULL.
void pond_get_population(int *koi, int *pads, int *motes);

/// Put the pond into (or out of) the look it wears while recording: warm
/// dark, dim water and every koi red, which no hour of the day and no
/// weather will ever do. Eased over about a second, and it holds the
/// pond's day still until it is switched off again.
void pond_set_recording(bool on);

/// Run the pond's day fast, for a demo: the hour steps on every couple of
/// seconds instead of every dwell, the weather steps with it so every sky
/// gets its turn, and the water, the koi and the pads move at speed to
/// match. Turning it on moves the day on straight away. Recording still
/// holds the day still, fast or not.
void pond_set_timelapse(bool on);

/// The scene on screen: the hour ("dusk", or "listening" while recording)
/// and the sky ("clear", "rain", ...). For logging.
const char *pond_hour_name(void);
const char *pond_sky_name(void);

#ifdef __cplusplus
}
#endif
