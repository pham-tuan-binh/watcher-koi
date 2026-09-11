#pragma once

/**
 * A koi pond, simulated and drawn in plain C.
 *
 * Nothing in here knows what it is running on. The pond gets a screen size
 * and a handful of callbacks at init, and from then on it wants to be
 * ticked every POND_TICK_MS. Each tick it moves the world; on a drawing
 * tick it also works out which rows of the screen changed and hands them
 * out as packed RGB565 bands through the render interface below. Whatever
 * owns the display gives it buffers to draw into and sends them on.
 *
 * One pond per program: the state is static, which keeps 1700 lines of
 * fixed-point simulation readable, and a Watcher has one glass.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Hard limits on how much the pond will simulate at once.
#define POND_MAX_KOI   12
#define POND_MAX_PADS  12
#define POND_MAX_MOTES 16

/// The pond is tuned for this tick; everything that moves is per tick.
#define POND_TICK_MS 40

/// Things the pond wants heard. Which ones, how often, follows the hour
/// and the weather; how they sound is somebody else's problem.
typedef enum {
    POND_SOUND_DROP = 0,   /*!< finger hits the water                  */
    POND_SOUND_SURFACE,    /*!< a koi noses the surface                */
    POND_SOUND_DISTANT,    /*!< something falls in over by the far bank */
    POND_SOUND_RAIN,       /*!< one raindrop                           */
    POND_SOUND_BIRD,
    POND_SOUND_INSECT,
    POND_SOUND_FROG,
    POND_SOUND_WIND,
    POND_SOUND_COUNT
} pond_sound_t;

/**
 * What the pond needs from the thing it is running on. All of it is
 * called from inside pond_tick(), on whatever task called that.
 *
 * Drawing goes out in bands: a band is `h` rows of `w` pixels starting at
 * screen column `x`, row `y`, packed one row after another with no
 * stride. The pond asks for a buffer with begin_band(), fills it, and hands
 * it back with end_band(). A buffer holds `width * band_rows` pixels. Two
 * buffers and a begin_band() that blocks until the older one has gone out
 * is enough to keep a DMA busy while the next band is drawn.
 */
typedef struct {
    void *ctx;                 /*!< passed back to every callback           */
    int width, height;         /*!< the screen, in pixels                   */
    int band_rows;             /*!< rows one band buffer holds, at least 16 */
    bool swap_bytes;           /*!< RGB565 with its two bytes swapped, the
                                    way a SPI panel takes it                */

    /// A free buffer of width * band_rows pixels. May block.
    uint16_t *(*begin_band)(void *ctx);
    /// Send the band. `px` is the buffer begin_band() gave out; the pond
    /// will not touch it again until begin_band() hands it back.
    void (*end_band)(void *ctx, int x, int y, int w, int h, const uint16_t *px);

    void (*sound)(void *ctx, pond_sound_t which);   /*!< may be NULL */
    void (*log)(void *ctx, const char *line);       /*!< may be NULL */
} pond_render_t;

typedef struct {
    int koi, pads, motes;      /*!< starting population; see pond_set_population */
    int dwell_sec;             /*!< how long an hour and a sky hold; 0 never moves on */
    uint32_t seed;             /*!< for the pond's own random numbers       */
} pond_config_t;

/// Set the pond up. Draws nothing: call pond_tick(now, true) for the first
/// frame. `render` is copied.
void pond_init(const pond_config_t *cfg, const pond_render_t *render);

/// One tick. `now_us` is any monotonic microsecond clock. The world moves
/// every tick; the screen is only touched when `draw` is set, so a host
/// that wants to save power draws every other tick and the fish do not
/// slow down.
void pond_tick(int64_t now_us, bool draw);

/// Drop a ripple at a screen coordinate. The koi swim over to investigate.
void pond_tap(int x, int y);

/// Step the zoom by `delta` detents (positive zooms in). Zoomed in, the view
/// follows a koi. Returns true if the zoom actually changed.
bool pond_zoom(int delta);

/// Change how much is simulated. Counts are clamped to the POND_MAX_*
/// limits; new koi, pads and motes are dropped into the pond and surplus
/// ones removed. This is how many exist, not how many you can see: they
/// roam past the rim into the dark.
void pond_set_population(int koi, int pads, int motes);
void pond_get_population(int *koi, int *pads, int *motes);

/// Put the pond into (or out of) the look it wears while recording: warm
/// dark, dim water and every koi red, which no hour and no weather will
/// ever do. Eased over about a second. Holds the pond's day still.
void pond_set_recording(bool on);

/// Run the day fast: an hour every couple of seconds, the weather stepping
/// with it, the water and the koi moving at speed to match. Turning it on
/// moves the day on straight away. Recording still holds the day still.
void pond_set_timelapse(bool on);

/// The scene on screen: the hour ("dusk", or "listening" while recording)
/// and the sky ("clear", "rain", ...).
const char *pond_hour_name(void);
const char *pond_sky_name(void);

/// How bright a backlight should be for the light on screen right now, 30
/// to 70 percent. Night asks for less than noon.
int pond_backlight_percent(void);

/// What the drawing has cost since the last reset, for a host that wants
/// to log it.
typedef struct {
    uint32_t frames_drawn;
    uint32_t cells_seen, cells_changed;   /*!< world cells on screen, and how many were repainted */
    uint64_t pixels_sent;                 /*!< through end_band() */
} pond_stats_t;
void pond_stats(pond_stats_t *out, bool reset);

#ifdef __cplusplus
}
#endif
