# Watcher Mochi

<p align="center">
  <img src="docs/pond.gif" width="412" alt="a pixel koi pond, tapped, rings spreading out">
</p>

A pixel koi pond for the [SenseCAP Watcher](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html). It runs its own day, dawn through night, with weather. Tap the glass and the koi swim over. Click the knob and it records the room to the SD card.

There are no image assets and no audio assets. Every frame is simulated and every sound is synthesised.

## What It Looks Like

<p align="center">
  <img src="docs/day.gif" width="412" alt="the pond moving through dawn, morning, noon, afternoon, dusk and night">
</p>

**A day.** Six hours in order, each held for three minutes, so a full day takes about eighteen. Hold a finger on the glass and it fast forwards, an hour every couple of seconds.

<p align="center">
  <img src="docs/day.png" width="900" alt="the same instant of the same pond at all six hours">
</p>

**The same instant at each hour.** Dawn, morning, noon on top; afternoon, dusk, night below. Only the light differs. A koi is the same orange at midnight as at noon, it is the light landing on it that changes.

<p align="center">
  <img src="docs/weather.gif" width="412" alt="the same afternoon under clear, overcast, rain and mist">
</p>

**Weather.** Clear, overcast, rain and mist, rolled on each hour. Rain puts rings on the water and a patter in the speaker. Mist greys the light and brings the motes up like fireflies.

<p align="center">
  <img src="docs/zoom.png" width="900" alt="the same moment at three zoom levels">
</p>

**Zoom.** Six detents on the knob. Wide open you see the whole pond. Closer in, the camera picks a koi and drifts after it. A koi under a lily pad glows through the leaf.

<p align="center">
  <img src="docs/drift.png" width="900" alt="the same pond forty seconds apart, four times">
</p>

**It drifts.** The koi roam past the rim and the pads wander, so how many you can count changes from minute to minute.

<p align="center">
  <img src="docs/recording.gif" width="412" alt="the pond turning red to record, then back again">
</p>

**Recording.** Click the knob and the pond turns red while it listens. Click again and there is a WAV on the card.

## Controls

| | |
| --- | --- |
| **Tap the screen** | A ripple where your finger landed, with a plop. Every koi swims over to see what fell in. |
| **Hold the screen** | Fast forward through the day. Let go and it carries on from wherever it got to. |
| **Turn the knob** | Zoom. |
| **Click the button** | Start recording. Click again to save. |
| **Long-press the button** | Deep sleep. |
| **Do nothing for a minute** | Deep sleep on its own, unless it is recording. A press of the button wakes it. |

## Build and Flash

You need a SenseCAP Watcher ([buy here, coupon 5EB420ZS](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html?sensecap_affiliate=3gToNR2&referring_service=link) — an affiliate link, so a little comes back to me), a USB-C cable and [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.4 or newer. A microSD card is only needed for recording.

```sh
idf.py build
idf.py flash monitor
```

`Ctrl+]` leaves the monitor. To hang the Watcher up, print [`print/case.3mf`](print/case.3mf): one part, 70 x 25 x 84 mm.

## How the Pond Is Drawn

Everything about the pond lives in [`main/pond.c`](main/pond.c) and [`main/pond.h`](main/pond.h), and nothing in there knows what it is running on. It is plain C99 with no platform includes; it compiles on a laptop with `cc -c main/pond.c`. The Watcher-specific part, about 200 lines, is in [`main/pond_port.c`](main/pond_port.c).

### A world of cells

The pond is a fixed **103 x 103 world** of world pixels. The screen is a camera window onto it: at the widest zoom one world pixel is a 4 x 4 block on the 412 pixel panel, so the world exactly fills the glass, and zooming in makes the blocks bigger and shows fewer of them.

Each visible cell carries two things, a **material** (water, koi body, koi marking, fin, pad, pad rim, flower, mote) and a **light level**, 0 to 255. Colour only happens at the very end:

```
per tick, into two small arrays the size of the visible window:

  water            ambient light: radial falloff, crossing swells
  + koi glow       halo, added to open water only
  + ripples        crest and trough, added to open water only
  + koi bodies     material + its own light
  + lily pads      material, dimming whatever was under it
  + underglow      a koi showing through a leaf
  + motes          material + halo
        |
        v
  quantise         index = material * 6 + level( light + dither )
        |
        v
  diff             which rows changed since last tick?
        |
        v
  bands            changed rows, packed, out through the render interface
```

Because everything that glows just *adds light*, the koi, the rings and the motes sit in one lighting model instead of three special cases. It is also why the day and the weather are cheap: they only change the palette, and the simulation never finds out.

### Light, not colour

An hour is ten bytes: what shadow fades to, the colour of the brightest step, the colour and strength of the light, saturation, ambient level, and three chances per tick of hearing a bird, a cricket or a frog. Weather is a set of percentages on top. The two compose into a `look_t`, every field of which is a byte, so a crossfade between two looks is one loop:

```c
for (size_t i = 0; i < sizeof(look_t); i++)
    out[i] = mix(a[i], b[i], t);   /* colours, speeds and rainfall together */
```

From a look the pond builds an **84-entry palette**: 14 materials times 6 light levels, RGB565, byte-swapped if the panel wants it that way. A 4 x 4 ordered dither breaks up the banding between the six levels. Recording is one more look through the same crossfade, and the only one that changes pigment rather than light, which is why it reads as a mode and not as a time of day.

### Only what changed

Every drawing tick, each visible cell is quantised to its palette index and compared with what it was. Runs of changed rows become **bands**: at most 40 screen rows tall, as wide as the union of the changed spans, widened to a multiple of four columns because panels like that. Each band is expanded from the palette straight into a buffer the host provides, packed, and handed back. The panel keeps whatever is not resent. On a calm night about 40% of the pixels go out per frame; in rain, about 60%.

### The render interface

That hand-over is the whole contract between the pond and the world:

```c
typedef struct {
    void *ctx;
    int width, height;         /* the screen */
    int band_rows;             /* rows one band buffer holds, at least 16 */
    bool swap_bytes;           /* RGB565 byte-swapped, as a SPI panel takes it */

    uint16_t *(*begin_band)(void *ctx);                 /* a free buffer; may block */
    void (*end_band)(void *ctx, int x, int y, int w, int h, const uint16_t *px);

    void (*sound)(void *ctx, pond_sound_t which);       /* optional */
    void (*log)(void *ctx, const char *line);           /* optional */
} pond_render_t;

void pond_init(const pond_config_t *cfg, const pond_render_t *render);
void pond_tick(int64_t now_us, bool draw);              /* every POND_TICK_MS */
void pond_tap(int x, int y);
bool pond_zoom(int delta);
```

To put the pond on something else, implement those four callbacks and call `pond_tick()` every 40 ms with any microsecond clock. `draw` says whether this tick should touch the screen; the world moves either way, so a host that wants to save power can draw every other tick and the fish do not slow down. The pond has its own random numbers, so the same seed gives the same pond on any chip.

On the Watcher, `begin_band()` hands out one of two 33 KB buffers in internal memory and blocks until the older one has gone out over SPI, so the DMA sends one band while the next is drawn. Everything else (tiers, backlight, deep sleep) is in `screen.c` and `board.c`.

### The things in it

- **Koi** are a 15 x 9 sprite sampled in body space, so they rotate with their heading for free. Markings are generated per fish: a few large plates, one per length band, so no two koi wear the same patch. The tail flick is one pixel.
- **Ripples** add light where `(d² - r²) / 2r` lands near zero, a cheap stand-in for distance to the ring, and take a little away just behind it. No square root in the loop.
- **Lily pads** are a circle with a wedge notch, a dark silhouette with a lit rim on the side facing the light. Free in the middle two thirds of the pond and steered back harder the further out they drift, so they never strand against the rim.
- **Everything fades with distance** from the middle, which is what makes it read as a pool in the dark rather than a flat background.

All of it is fixed-point integer maths on a 256-entry sine table.

## How It Sounds

Every sound in [`main/sound.c`](main/sound.c) is synthesised from the physics of what makes the noise. A plop is an air bubble trapped under the surface: a filtered click, a few milliseconds of nothing while the crater forms, then a decaying sine at the bubble's [Minnaert resonance](https://en.wikipedia.org/wiki/Minnaert_resonance), pitch rising as it rings ([van den Doel, 2005](https://dl.acm.org/doi/10.1145/1101530.1101554)). Voices are specified by bubble radius, not frequency. A raindrop traps a smaller bubble than a fingertip, so it rings near 2.2 kHz where the tap plop rings near 1.1 kHz.

Birds, crickets and frogs are a tone, an envelope and a warble. Wind is filtered noise with the rumble subtracted back out, because a speaker this size cannot move it anyway. They mix through two damped combs, just enough reverb to put the pond in a dark room. The codec is powered only while something is sounding.

## Recording

Click the knob. The card is mounted on the first click, not at boot, so a Watcher with no card costs nothing and one pushed in later works. The pond only turns red once a file is genuinely open. Recordings land in `/POND` on the card as `REC00001.WAV` upwards, 16 kHz mono 16-bit. The pond goes silent while recording, because the mic and the speaker are millimetres apart, and its day holds still so a recording never gets a sunrise halfway through. A recording nobody stops saves itself at `MOCHI_REC_MAX_SEC`.

## Power

The Watcher runs on a 400 mAh battery. What the firmware does about that, roughly in order of how much it saves:

- **Rails it does not need are off**: the vision chip and camera, the Grove socket and the battery ADC never come up, and the SD card's rail is on only while a file is open.
- **The backlight follows the scene**, 30% on a rainy night to 70% on a clear noon, and halves after 20 s without a touch.
- **The pond draws itself onto the panel**, only the rows that changed, from internal memory. LVGL still runs the touch panel and the knob but no longer draws.
- **Idle draws every other frame.** The world keeps moving at 25 Hz.
- **The codec sleeps between sounds**, amplifier rail and all.
- **The CPU drops to 80 MHz when idle**, which after the above is most of each frame.
- **Deep sleep after a minute**, waking on the knob button.

The frame log prints once every 30 s with how long a tick takes, how many cells changed and what share of the pixels were sent. See `sdkconfig.defaults` for the profiling options that add time per clock and per task.

## Configuration

`idf.py menuconfig`, under **Mochi**:

| Option | Default | |
| --- | --- | --- |
| `MOCHI_DEEP_SLEEP_TIMEOUT_SEC` | 60 | Idle seconds before deep sleep |
| `MOCHI_REC_MAX_SEC` | 600 | Longest recording before it saves itself |
| `MOCHI_SCENE_DWELL_SEC` | 180 | How long one hour and weather holds; 0 never moves on |
| `MOCHI_POND_KOI_COUNT` | 8 | Koi simulated (max 12) |
| `MOCHI_POND_LILY_COUNT` | 5 | Lily pads simulated (max 12) |
| `MOCHI_POND_MOTE_COUNT` | 7 | Drifting motes (max 16) |

Populations are starting points; `pond_set_population()` changes them at runtime.

## License

Apache License 2.0, see [LICENSE](LICENSE).
