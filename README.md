# Watcher Mochi

<p align="center">
  <img src="docs/pond.gif" width="412" alt="a pixel koi pond, tapped, rings spreading out">
</p>

A pixel koi pond for the [SenseCAP Watcher](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html). Dark water, glowing rings, koi that come over when you tap the glass. Click the knob and it turns to autumn and records the room.

Nothing is loaded from storage. There are no image assets and no audio assets — every frame is simulated and every sound is synthesised, so the firmware is the whole thing.

## What You Need

- SenseCap Watcher: [Buy here - 69$ - Coupon: 5EB420ZS](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html?sensecap_affiliate=3gToNR2&referring_service=link)
- A USB-C cable
- A computer with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.4+ installed

A microSD card, but only if you want to record — everything else runs without one.

❤️ **If you want to buy a SenseCap Watcher, consider buying with the link or coupon above**. It's an affiliate link so I'll get a small percentage of your order as appreciation ^^

## Build and Flash

Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.4 or newer for your platform, connect the Watcher over USB-C, then from this folder:

```sh
idf.py build
idf.py flash monitor
```

`flash` uploads the firmware, `monitor` shows the serial log. `Ctrl+]` exits the monitor.

## Controls

| | |
| --- | --- |
| **Tap the screen** | A ripple lands where your finger did, with a wet plop. Three rings spread out and every koi swims over to see what fell in. |
| **Turn the knob** | Zoom, six steps. Wide open you get the whole pond; closer in the camera picks a koi and drifts after it. |
| **Click the button** | The pond turns to autumn and starts recording. Click again to save the WAV to the card. |
| **Press the button** | Wakes it from sleep. |
| **Long-press the button** | Straight to deep sleep, closing any recording on the way out. |
| **Do nothing for 5 minutes** | Deep sleep on its own — unless it is recording, which is not being idle. |

Left alone it keeps going: the koi wander, the lily pads drift, one of the fish noses the surface every so often, and every now and then something falls in over by the far bank.

## Recording

Click the knob button and the pond turns: the water drops to dusk, the lily pads rust over, the dark goes warm, and every koi comes up red. That is the Watcher listening to the room. Click again and the pond cools back to green — and a WAV file is sitting on the card.

The turn is eased over about a second and a third, on a smoothstep, with a ring going out across the water as it starts, so the season arrives as something happening to the pond rather than a palette being swapped underneath it. Nothing in the simulation knows it happened. Colour in this pond is only ever `ramp[material][light]`, so autumn is a second table of fully-lit colours crossfaded into the first — 84 ramp entries rebuilt on the frames where it moves, which is nothing next to a frame of water:

```c
void pond_set_autumn(bool on);
```

**The pond goes silent while it records.** The Watcher's mic and its speaker are millimetres apart, so a plop played during a recording is a plop *in* the recording — every voice is dropped and anything mid-ring is faded out inside one 16 ms block. A pond that stops talking the moment it starts listening also happens to be a clearer signal than any icon would be, so the two wants agree.

Recordings land in `/POND` on the card, numbered `REC00001.WAV` upwards — 16 kHz mono 16-bit, the codec's native rate, which is about 2 MB a minute. The card is mounted on the first click rather than at boot, so a Watcher with no card in it costs nothing and one pushed in later just works. No card, and a click does nothing at all: the pond only turns red once a file is actually open, so the colour on the glass is the recording's own state and never a guess at it.

A few things in [`main/recorder.c`](main/recorder.c) worth knowing:

- **Two tasks, with a ring between them.** 32 KB/s is nothing for a card, but latency is: a FAT write can stall for tens of milliseconds allocating a cluster, and the I2S input only has about 90 ms of DMA behind it. One task that read a chunk and then wrote it would drop samples on any card having a bad day. So a reader does nothing but pull from the mic, a writer empties whatever has piled up, and the ring between them is four seconds deep in PSRAM. The reader outranks the writer, because a late write is absorbed and a late read is a hole.
- **The vision chip has to be told to let go of the bus.** The card and the Himax share SPI2 with a chip-select each. The BSP parks the card's CS high before it talks to the vision chip, and nothing does the reverse — so on a build like this one, which never brings the Himax up, its CS floats while the chip sits there powered: the card's own clock edges select it, it drives MISO against the card, and every read comes back `data CRC failed`. `board_init()` parks it high once and the card has the bus to itself.
- **The header is patched on the way out.** WAV wants its two sizes at the front of the file and neither is known until the end, so it goes down as zeros and is rewritten when the file closes.
- **It always closes the file.** A long press finishes the recording and waits for it before cutting power, the inactivity timeout will not fire while a recording is running, and a recording nobody comes back to stop saves itself at `MOCHI_REC_MAX_SEC` (ten minutes by default).

## Zoom

<p align="center">
  <img src="docs/zoom.png" width="900" alt="the same moment at three zoom levels">
</p>

The same instant at three of the six detents. The pond is a fixed world and the screen is a camera window into it, so zooming changes how many screen pixels one world pixel gets — you see less of the pond, and what you do see is chunkier. Past the widest step the camera latches onto a koi and follows it, aiming a little ahead so the easing lag cancels out. A koi that swims under a lily pad glows through the leaf rather than disappearing (visible on the left pad in the last shot).

## A Pond That Moves

<p align="center">
  <img src="docs/drift.png" width="900" alt="the same pond forty seconds apart, four times">
</p>

The same pond, roughly forty seconds apart each time. These were rendered with three koi and five lily pads — the firmware now starts with eight koi — and that whole count is being simulated in every one of the frames, but the koi roam past the rim and the pads drift, and everything dims with distance from the middle, so the number you can actually *count* moves around. Sometimes five pads, sometimes four; sometimes three fish, sometimes one and a faint shape at the edge.

Counts are a runtime number, not a compile-time one:

```c
void pond_set_population(int koi, int pads, int motes);
void pond_get_population(int *koi, int *pads, int *motes);
```

Left to itself a drifting leaf is a 2D random walk, and a random walk spends most of its time far from where it started — the first version of this had every lily pad stranded against the rim within two minutes. So pads are free in the middle two-thirds of the pond and steered back harder the further out they get, and they nudge each other apart so they crowd without stacking.

## How It's Drawn

Everything lives in [`main/pond.c`](main/pond.c). The pond is a fixed **103×103 world** of world pixels — a quarter of the 412×412 panel at the widest zoom, which is where the chunky pixels come from. Each visible cell carries two things, a **material** and a **light level**, and colour only happens at the very end:

```
per frame, into two buffers the size of the visible window:

  water            ambient light: radial falloff, crossing swells
  + koi glow       halo, added to open water only
  + ripples        crest and trough, added to open water only
  + koi bodies     material + its own light
  + lily pads      material, dimming whatever was under it
  + underglow      a koi showing through a leaf
  + motes          material + halo
        │
        ▼
  blit             colour = ramp[material][ (light + dither) → 0..5 ]
        │
        ▼
  canvas           each cell painted as an s_pix × s_pix block
```

Because everything that glows just *adds light*, the koi, the rings and the motes all sit in one lighting model instead of being three special cases. Light falls off towards the rim, which is what makes the pond read as a pool in the dark rather than a flat background, and a 4×4 [ordered dither](https://en.wikipedia.org/wiki/Ordered_dithering) breaks up the banding between the six levels.

Some details worth knowing:

- A tap spawns three staggered rings. Each adds light where `(d² − r²) / 2r` — a cheap stand-in for the distance to the ring — lands near zero, and takes a little away just behind it for the trough. No `sqrt` in the inner loop.
- Koi are an 11×7 sprite sampled in *body* space, so they rotate with their heading for free. The tail flick is capped at one pixel; at two it tears away from the body.
- Lily pads are a circle with a wedge notch, drawn as a dark silhouette with a lit rim on the side facing the light.

All of it is fixed-point integer maths on a 256-entry sine table. A frame costs a few milliseconds at 25 fps and leaves the CPU mostly idle.

## How It Sounds

Every sound in [`main/sound.c`](main/sound.c) is synthesised, from the physics of what actually makes the noise.

The plop of something hitting water is not the drop. Phillips, Agarwal and Jordan filmed it with high-speed cameras and found the sound is driven by a small **air bubble trapped under the surface**: the impact makes a brief click, the crater takes a few milliseconds to form, then the entrapped bubble rings and drives the water surface like a piston ([Scientific Reports, 2018](https://www.nature.com/articles/s41598-018-27913-0)). Each voice here is that same three-part event — a filtered noise click, a short gap, then a decaying sine.

Two results give the rest for free:

- **[Minnaert's 1933 result](https://en.wikipedia.org/wiki/Minnaert_resonance)**, that a bubble in water resonates at `f₀ · r ≈ 3.26 Hz·m`. So voices are specified by *bubble radius*, not frequency, and the pitch falls out of the physics. A fingertip-sized pocket of air (3–4 mm) rings around 800–1000 Hz.
- **[Van den Doel's liquid sound model](https://dl.acm.org/doi/10.1145/1101530.1101554)** (ACM TAP, 2005), where the pitch rises as the bubble rings: `f(t) = f₀ · (1 + ξ · d · t)` against an `e^(−d·t)` decay, with `ξ ≈ 0.1` found experimentally. That rise is the part your ear reads as *water*, and it costs one add per sample.

Four voices — the tap plop, a softer lower one when a koi noses the surface, a quiet far-off drop with a long tail, and a dry noise tick for each knob detent. They mix in one task through two damped feedback combs, just enough reverb to put the pond in a dark room, then a `tanh` soft limiter so several drops at once bend rather than clip. Bubble radius and decay are randomised per hit, so no two plops are the same.

For the record, a real dripping tap traps a bubble ten times smaller and plinks up near 9 kHz — above the Nyquist limit of this 16 kHz codec, and well past what its little speaker could move. Bigger, lower bubbles are both the right sound for a pond and the only one this hardware can make.

The audio task only streams while something is sounding, so silence costs nothing.

## Configuration

`idf.py menuconfig`, under the **Mochi** menu:

| Option | Default | |
| --- | --- | --- |
| `MOCHI_DEEP_SLEEP_TIMEOUT_SEC` | 300 | Idle seconds before deep sleep |
| `MOCHI_REC_MAX_SEC` | 600 | Longest recording before it saves itself |
| `MOCHI_POND_KOI_COUNT` | 8 | Koi simulated (max 12) |
| `MOCHI_POND_LILY_COUNT` | 5 | Lily pads simulated (max 12) |
| `MOCHI_POND_MOTE_COUNT` | 7 | Drifting motes (max 16) |

These are starting values — `pond_set_population()` changes any of them at runtime. Defaults are in `sdkconfig.defaults`.

## License

The firmware source code is licensed under the [Apache License 2.0](LICENSE).
