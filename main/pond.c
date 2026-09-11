/**
 * A small pixel-art koi pond, lit like a cave.
 *
 * The pond is a fixed WORLD_W x WORLD_W world of "world pixels". The screen
 * is a camera window into it: every world pixel is drawn as a s_pix x s_pix
 * block, so the zoom level decides both how much of the pond is visible and
 * how chunky it looks. Fully zoomed out the whole pond fits the panel;
 * zoomed in the camera drifts after one of the koi.
 *
 * Two buffers are kept per visible cell: a material (water, pad, koi, ...)
 * and a light level. Colour only comes together at blit time, where each
 * material is looked up in its own dark-to-lit ramp. Anything that glows
 * just adds light, so the koi, the ripples and the motes all sit in the
 * same lighting model.
 *
 * Per frame: ambient water light -> koi glow -> ripple light -> koi bodies
 * -> lily pads -> motes -> blit. Glow is only added to open water so a ring
 * never washes out a fish or a pad.
 *
 * How many koi, pads and motes exist is a runtime number, not a compile
 * time one. They are free to roam past the rim, and everything fades out
 * with distance from the middle of the pond, so the count you can see
 * drifts either side of the count being simulated.
 *
 * Because colour is only ever a lookup, what the pond looks like is a
 * small block of bytes (a light, a saturation, an ambient level, and how
 * hard the water moves) rather than anything the simulation knows about.
 * A time of day composes with a weather into one of those blocks, and
 * every change of scene, the red one it wears while recording included, is
 * the same byte-wise crossfade between two of them.
 *
 * None of this knows what it is running on. The screen size, the buffers
 * the pixels go into and what happens to them afterwards, the sounds and
 * the log all come in through the pond_render_t in pond.h. This file has
 * no platform includes and compiles anywhere.
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pond.h"

// --- The world outside ---
//
// Everything the pond needs from the platform arrives through s_rc, set at
// init and never touched again. See pond.h for the contract.

static pond_render_t s_rc;
static pond_config_t s_cfg;
static int64_t s_now_us;          /* the clock, as of the current tick */

static void plog(const char *fmt, ...)
{
    if (!s_rc.log)
        return;
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s_rc.log(s_rc.ctx, line);
}

static void emit(pond_sound_t which)
{
    if (s_rc.sound)
        s_rc.sound(s_rc.ctx, which);
}

/// RGB565, byte-swapped if the panel wants it that way. Decided once, when
/// the palette is built, so it costs nothing per pixel.
static uint16_t pack565(int r, int g, int b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return s_rc.swap_bytes ? (uint16_t)((v << 8) | (v >> 8)) : v;
}

/// xorshift32: plenty for where a leaf drifts, and the same on every chip.
static uint32_t s_rng = 2463534242u;
static uint32_t rnd(uint32_t n)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return n ? s_rng % n : 0;
}

// --- Geometry ---
//
// World coordinates are in world pixels. The world is WORLD_W across
// whatever the screen, which fixes the size of every buffer below; at the
// widest zoom one world pixel covers s_pix_min screen pixels, the smallest
// whole number that fits the world on the screen. On a 412 pixel panel that
// is 4, and the world exactly fills it.

#define WORLD_W   103
#define WORLD_CX  (WORLD_W / 2)
#define WORLD_CY  (WORLD_W / 2)
#define POND_R    (WORLD_W / 2)               /* the display is round */

/* Everything fades out with distance, reaching black a little past the rim.
 * Roaming limits sit inside that, so wanderers dim rather than blink out. */
#define FADE_INNER (POND_R * 3 / 5)   /* lit normally out to here ... */
#define FADE_R     (POND_R + 12)      /* ... then down to nothing by here */
#define KOI_ROAM  (POND_R + 4)
#define PAD_ROAM  (POND_R + 3)
#define MOTE_ROAM (POND_R + 2)

/* Zoom detents, in quarters of the widest zoom's pixel size: 1x, 1.25x,
 * 1.5x, 2x, 2.5x, 3.25x. On the Watcher that is 4, 5, 6, 8, 10 and 13
 * screen pixels per world pixel. */
static const uint8_t ZOOM_NUM[] = { 4, 5, 6, 8, 10, 13 };
#define ZOOM_STEPS ((int)(sizeof ZOOM_NUM / sizeof ZOOM_NUM[0]))
static int s_pix_min;

#define MAX_RIPPLES 12

// --- Materials and their light ramps ---

enum {
    MAT_WATER = 0,
    MAT_PAD, MAT_PAD_RIM, MAT_FLOWER,
    MAT_KOI_A_M, MAT_KOI_A_A, MAT_KOI_A_F,
    MAT_KOI_B_M, MAT_KOI_B_A, MAT_KOI_B_F,
    MAT_KOI_C_M, MAT_KOI_C_A, MAT_KOI_C_F,
    MAT_MOTE,
    MAT_COUNT
};

#define LEVELS 6

/* The pond's pigments: the colour of each material under a full, white
 * light. What an hour of the day or a turn of the weather does to them is
 * a matter for the looks below. These do not change. */
static const uint8_t MAT_RGB[MAT_COUNT][3] = {
    {  70, 150, 165 },                                   /* water          */
    {  74, 140,  70 }, { 156, 204, 112 }, { 245, 175, 205 },
    { 255, 138,  48 }, { 250, 246, 240 }, { 236, 182, 150 },
    { 238,  70,  70 }, { 248, 242, 238 }, { 232, 154, 154 },
    { 120, 132, 160 }, { 215, 222, 235 }, { 168, 186, 208 },
    { 205, 245, 225 },                                   /* drifting mote  */
};

/* How much of the base colour survives at each light level, in 1/256ths. */
static const uint16_t LEVEL_MIX[LEVELS] = { 26, 56, 100, 150, 205, 256 };

static uint16_t s_pal[MAT_COUNT * LEVELS];
static bool s_pal_dirty;

// --- Looks: the time of day, the weather, and the pond listening ---
//
// An hour of the day does not repaint the pond, it relights it. A koi is
// the same orange at midnight as at noon; what changes is the light landing
// on it. So an hour carries a light and never a colour for the fish: what
// shadow fades to, what the brightest step glows, the colour of the
// illumination itself, and how hard that illumination stains what it falls
// on. Weather is then a handful of percentages laid over an hour --
// overcast takes the colour and the light down and lifts the shadows grey,
// rain takes more and puts rings on the water, mist keeps the light but
// eats the contrast.
//
// The two compose into a look_t, which is what actually gets rendered, and
// a look is nothing but bytes: crossfading between two of them is one pass
// over the struct. That is the entire transition machinery, and it is the
// same machinery whether the pond is sliding from dusk into night over six
// seconds or going red because you asked it to record.

typedef struct {
    uint8_t pigment[MAT_COUNT][3]; /* fully lit colour of every material   */
    uint8_t night[3];              /* what everything fades to in shadow   */
    uint8_t glow[3];               /* colour of the brightest light step   */
    uint8_t light[3];              /* colour of the illumination itself    */
    uint8_t light_mix;             /* how far the light stains the pigment */
    uint8_t sat;                   /* 255 keeps the pigment, 0 greys it    */
    uint8_t ambient;               /* open water light at the pond centre  */
    uint8_t swell;                 /* surface movement, 128 = as designed  */
    uint8_t drift;                 /* pad and mote speed, 128 = as designed*/
    uint8_t rain;                  /* raindrops: chance in 256 per frame   */
    uint8_t mote_gain;             /* how hard the motes burn, 128 = as is */
    uint8_t birds, insects, frogs; /* chance in 2048 per frame, each       */
    uint8_t wind;                  /* likewise, for a gust over the water  */
} look_t;

/* Every field is a byte, so a look crossfades as one flat run of bytes.
 * This is what keeps that true if a wider field is ever added below. */
_Static_assert(sizeof(look_t) == MAT_COUNT * 3 + 9 + 11,
               "look_t has to stay all bytes for look_lerp");

/* The light of one hour, and what can be heard in it. No pigments: see
 * above. The three rates are chances in 2048 per frame, so at 25 fps a
 * value of 40 is about one every two seconds. */
typedef struct {
    const char *name;
    uint8_t night[3], glow[3], light[3];
    uint8_t light_mix, sat, ambient, mote_gain;
    uint8_t birds, insects, frogs;
} hour_t;

/* Six hours around a day. Night is the pond as it was first built, a cave
 * lit blue, and the rest of the day opens up from there. */
static const hour_t HOURS[] = {
    /*                night          glow               light         mix  sat  amb mote  brd ins frg */
    { "dawn",      { 13,  9, 14 }, { 255, 198, 188 }, { 255, 162, 140 }, 100, 180,  88, 150,  46,  2, 10 },
    { "morning",   {  6, 11, 18 }, { 225, 245, 255 }, { 198, 230, 255 },  55, 245, 138,  70,  32,  4,  0 },
    { "noon",      {  9, 15, 18 }, { 255, 252, 242 }, { 255, 250, 232 },  40, 255, 172,  40,   9, 12,  0 },
    { "afternoon", { 14, 11,  9 }, { 255, 222, 165 }, { 255, 203, 128 },  80, 250, 148,  80,  14, 17,  3 },
    { "dusk",      { 14,  7, 11 }, { 255, 168, 112 }, { 255, 133,  92 }, 115, 200,  82, 170,   7, 27, 24 },
    { "night",     {  4,  8, 13 }, { 165, 225, 235 }, {  95, 140, 225 }, 115, 120,  50, 255,   0, 35, 31 },
};
#define HOUR_COUNT ((int)(sizeof HOURS / sizeof HOURS[0]))

/* Weather, as percentages of whatever hour it lands on. Birds and insects
 * shut up in the wet, which is what critter_pct is for. */
typedef struct {
    const char *name;
    uint8_t sat_pct, ambient_pct, mote_pct, critter_pct;
    uint8_t haze[3], haze_mix;     /* the light drifts towards this */
    uint8_t swell, drift, rain, wind;
} weather_t;

enum { SKY_CLEAR = 0, SKY_OVERCAST, SKY_RAIN, SKY_MIST, SKY_COUNT };

static const weather_t WEATHER[] = {
    /*           sat  amb mote crit        haze         hz  swl drf rain wind */
    { "clear",   100, 100, 100, 100, { 255, 255, 255 },  0, 128, 128,  0,   0 },
    { "overcast", 70,  78, 115,  72, { 150, 158, 170 }, 70, 150, 150,  0,  11 },
    { "rain",     56,  62,  65,  22, { 138, 148, 160 }, 95, 200, 175, 90,  18 },
    { "mist",     72,  90, 145,  58, { 198, 208, 208 }, 92,  90,  95,  0,   5 },
};
_Static_assert((int)(sizeof WEATHER / sizeof WEATHER[0]) == SKY_COUNT,
               "WEATHER must match the sky enum");

/* Mostly clear: the wet skies are the exception, not the rule. */
static const uint8_t SKY_WEIGHT[SKY_COUNT] = { 110, 45, 28, 22 };

/* The pond listening. This one is a pigment change and not a light change:
 * every koi goes red, which no hour and no weather would ever do, and that
 * is exactly why it reads as a mode rather than as a time of day. */
static const uint8_t REC_PIGMENT[MAT_COUNT][3] = {
    { 104,  84,  76 },                                   /* dusk on brown  */
    { 146,  86,  38 }, { 232, 164,  76 }, { 255, 214, 150 },
    { 238,  48,  36 }, { 255, 168, 104 }, { 172,  34,  28 },
    { 212,  26,  40 }, { 255, 140,  96 }, { 150,  20,  30 },
    { 250,  74,  34 }, { 255, 186, 120 }, { 184,  40,  24 },
    { 255, 204, 148 },                                   /* ember mote     */
};

/* No birds, no crickets, no frogs: the sound module drops everything while
 * a file is open anyway, and a look that asked for them would be lying. */
static const hour_t REC_HOUR = {
    "listening", { 15, 7, 6 }, { 255, 194, 126 }, { 255, 170, 120 },
    60, 240, 98, 120, 0, 0, 0
};

/* A scene change is eased over this many frames: a sky takes its time, the
 * pond answering a button press should not. */
#define FADE_SKY_FRAMES 150
#define FADE_REC_FRAMES 32

/* Wall clock, not a frame count. A frame is nominally FRAME_MS but the
 * timer only fires once the last one has been drawn, so the real period
 * runs nearer 49 ms, and counting frames made every dwell 22% long. */
#define DWELL_US ((int64_t)s_cfg.dwell_sec * 1000000)

/* Fast forward, which a finger held on the glass turns on. The day steps on
 * every couple of seconds instead of every dwell, with a crossfade short
 * enough to finish before the next one starts, and the water runs at speed
 * underneath it so the pond reads as a time lapse rather than as a slide
 * show of itself. Twelve scenes, about half a minute, and you have seen
 * every hour and every sky. */
#define TIMELAPSE_DWELL_US ((int64_t)2400000)
#define FADE_FAST_FRAMES   28
#define TIMELAPSE_STEPS    3      /* passes of world motion per drawn frame */

static look_t s_look;          /* what is being rendered right now */
static look_t s_look_from, s_look_to;
static int s_fade_at, s_fade_frames;
static int s_hour, s_sky;
static bool s_recording;
static bool s_timelapse;
static int64_t s_scene_due;

static uint8_t mix(uint8_t a, uint8_t b, int t)
{
    return (uint8_t)((a * (256 - t) + b * t) >> 8);
}

static uint8_t pct_u8(int v, int pct)
{
    int r = v * pct / 100;
    return (uint8_t)(r > 255 ? 255 : r);
}

/// Work out what an hour under a given sky actually renders as.
static void look_compose(look_t *out, const hour_t *h, const weather_t *w)
{
    memcpy(out->pigment, MAT_RGB, sizeof out->pigment);

    for (int c = 0; c < 3; c++) {
        /* haze lifts the shadows only half as far as it washes out the
         * light: fog greys what you can see before it fills in what you
         * cannot */
        out->night[c] = mix(h->night[c], w->haze[c], w->haze_mix / 2);
        out->glow[c] = mix(h->glow[c], w->haze[c], w->haze_mix);
        out->light[c] = mix(h->light[c], w->haze[c], w->haze_mix);
    }

    out->light_mix = h->light_mix;
    out->sat = pct_u8(h->sat, w->sat_pct);
    out->ambient = pct_u8(h->ambient, w->ambient_pct);
    out->mote_gain = pct_u8(h->mote_gain, w->mote_pct);
    out->swell = w->swell;
    out->drift = w->drift;
    out->rain = w->rain;
    out->wind = w->wind;

    out->birds = pct_u8(h->birds, w->critter_pct);
    out->insects = pct_u8(h->insects, w->critter_pct);
    out->frogs = pct_u8(h->frogs, w->critter_pct);
}

/// Crossfade two looks. All bytes, so this does not care what the fields
/// mean: colours, speeds and rainfall all cross over together.
static void look_lerp(look_t *out, const look_t *a, const look_t *b, int t)
{
    uint8_t *o = (uint8_t *)out;
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof(look_t); i++)
        o[i] = mix(pa[i], pb[i], t);
}

/// Rebuild every ramp for a look: pigment, washed out by how grey the sky
/// is and stained by the colour of the light, then faded towards that
/// look's own shadow across the six steps.
static void palette_build(const look_t *lk)
{
    for (int m = 0; m < MAT_COUNT; m++) {
        int lum = (lk->pigment[m][0] * 77 + lk->pigment[m][1] * 150
                 + lk->pigment[m][2] * 29) >> 8;

        uint8_t base[3];
        for (int c = 0; c < 3; c++) {
            uint8_t p = mix((uint8_t)lum, lk->pigment[m][c], lk->sat);
            base[c] = mix(p, lk->light[c], lk->light_mix);
        }

        for (int l = 0; l < LEVELS; l++) {
            int k = LEVEL_MIX[l];
            uint8_t r = mix(lk->night[0], base[0], k);
            uint8_t g = mix(lk->night[1], base[1], k);
            uint8_t b = mix(lk->night[2], base[2], k);
            if (l == LEVELS - 1) {
                /* brightest step picks up a little of the glow's own colour */
                r = mix(r, lk->glow[0], 36);
                g = mix(g, lk->glow[1], 36);
                b = mix(b, lk->glow[2], 36);
            }
            s_pal[m * LEVELS + l] = pack565(r, g, b);
        }
    }
    s_pal_dirty = true;   /* every cell on screen has just changed colour */
}

/// Smoothstep on 0..256, so a turn eases in and out rather than starting
/// and stopping dead.
static int ease_256(int t)
{
    return (t * t * (768 - 2 * t)) >> 16;
}

/// Carry a crossfade on by one frame. Eighty four ramp entries is nothing
/// next to a frame of water, and this only runs while something is turning.
static void look_step(void)
{
    if (s_fade_at >= s_fade_frames)
        return;

    s_fade_at++;
    look_lerp(&s_look, &s_look_from, &s_look_to,
              ease_256(256 * s_fade_at / s_fade_frames));
    palette_build(&s_look);
}

// --- Koi sprite ---
// Body space: x runs tail (0) -> nose (KOI_W - 1), y is across the body.
// 'm' main colour, 'a' marking, 'f' fin, '.' water.
//
// The silhouette follows how a koi is actually built: fusiform, narrow at
// the snout, broadest across the shoulder a quarter of the way back, then
// tapering the whole length of the body to a narrow caudal peduncle. A fish
// widest at its middle reads as a rugby ball, which is the fault koi judges
// mark down.
//
// The tail is a flat blade, not a fan. A fish's caudal fin stands vertical
// and beats side to side, so from directly above you are looking at its
// edge. The fork everyone draws is a side-on view and is not visible from
// up here.
//
// This is only the blank. The markings are not in the art: each koi gets
// its own in koi_pattern(), because every koi wearing the same patch in the
// same place reads as a stripe down the fish.

#define KOI_W 15
#define KOI_H 9
#define KOI_PIVOT 7    /* body-space x the koi turns about */
#define KOI_REACH 9    /* body bounding box half-size, world pixels */
#define KOI_GLOW_R 12  /* halo cast on the surrounding water */
#define KOI_BODY_X0 4  /* first body column; everything behind it is tail */

static const char *const KOI_ART[KOI_H] = {
    "...............",
    "...............",
    ".........mmmm..",
    "ffff.mmmmmmmmm.",
    "ffffmmmmmmmmmmm",
    "ffff.mmmmmmmmm.",
    ".........mmmm..",
    "...............",
    "...............",
};

static uint8_t s_koi_sprite[KOI_H][KOI_W];

/* main, accent, tail, indexed by sprite value - 1 */
static const uint8_t KOI_MAT[3][3] = {
    { MAT_KOI_A_M, MAT_KOI_A_A, MAT_KOI_A_F },
    { MAT_KOI_B_M, MAT_KOI_B_A, MAT_KOI_B_F },
    { MAT_KOI_C_M, MAT_KOI_C_A, MAT_KOI_C_F },
};

// --- Trig, all fixed point (256 = 1.0, angles are 0..255 = full turn) ---

static int16_t s_sin[256];

#define SIN(a) (s_sin[(uint8_t)(a)])
#define COS(a) (s_sin[(uint8_t)((a) + 64)])

static void trig_init(void)
{
    for (int i = 0; i < 256; i++)
        s_sin[i] = (int16_t)lroundf(sinf((float)i * 2.0f * (float)M_PI / 256.0f) * 256.0f);
}

static uint8_t angle_of(int dx, int dy)
{
    float a = atan2f((float)dy, (float)dx) * 256.0f / (2.0f * (float)M_PI);
    return (uint8_t)(int)lroundf(a);
}

static int isqrt32(int32_t v)
{
    if (v <= 0)
        return 0;
    int32_t x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (int)x;
}

/// How lit a thing at (wx, wy) should be, 256 in the middle of the pond down
/// to 0 out past the rim. Everything that has its own colour is scaled by
/// this, so wanderers fade into the dark instead of floating in the void.
static int rim_fade(int wx, int wy)
{
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    int d = isqrt32((int32_t)dx * dx + (int32_t)dy * dy);
    if (d <= FADE_INNER)
        return 256;                    /* full brightness across the middle */
    int f = 256 - (256 * (d - FADE_INNER)) / (FADE_R - FADE_INNER);
    return f < 0 ? 0 : f;
}

// --- Entities, all positioned in world coordinates, 8.8 fixed point ---

typedef struct {
    int32_t x, y;
    uint8_t heading;   /* 0..255 */
    uint8_t phase;     /* tail wiggle phase */
    int16_t speed;     /* 8.8 world pixels per frame */
    int8_t  turn;
    uint8_t type;
    uint16_t boost;    /* frames left chasing the last tap */
    int16_t tx, ty;    /* what the koi is swimming towards */
    uint8_t skin[KOI_H][KOI_W];   /* this fish's own markings */
} koi_t;

typedef struct {
    int32_t x, y;
    uint8_t r;
    uint8_t notch;     /* direction of the wedge cut out of the pad */
    uint8_t phase;     /* bob phase */
    uint8_t heading;   /* which way it is drifting */
    int16_t speed;
    bool flower;
} pad_t;

typedef struct {
    int32_t x, y;
    uint8_t heading;
    uint8_t phase;
    int16_t speed;
} mote_t;

typedef struct {
    int16_t x, y;
    int16_t age;       /* negative while the ripple is still waiting to start */
    int16_t life;
    bool active;
} ripple_t;

#define RIPPLE_SPEED 340   /* 8.8 world pixels per frame */
#define RIPPLE_LIFE  46

static koi_t s_koi[POND_MAX_KOI];
static pad_t s_pads[POND_MAX_PADS];
static mote_t s_motes[POND_MAX_MOTES];
static ripple_t s_ripples[MAX_RIPPLES];

/// How much is simulated right now. Runtime, not baked in.
static int s_koi_count;
static int s_pad_count;
static int s_mote_count;

static uint8_t s_mat[WORLD_W][WORLD_W];
static int16_t s_light[WORLD_W][WORLD_W];
static int8_t s_dither[4][4];


/* What each visible cell was painted with last frame, as a palette index,
 * so the blit can find the rows that actually changed and leave the rest
 * of the panel alone. */
static uint8_t s_prev_idx[WORLD_W][WORLD_W];
static bool s_prev_valid;      /* false forces a full repaint next blit */
static uint32_t s_dirty_cells, s_seen_cells;     /* for the frame log */
static uint64_t s_flushed_px;
static uint32_t s_drawn_frames;
static uint32_t s_frame;
static uint16_t s_next_surface;
static uint16_t s_next_distant;

// --- Camera ---

static int s_zoom;                  /* index into ZOOM_NUM */
static int s_pix = 4;               /* screen pixels per world pixel */
static int s_gw = WORLD_W;          /* visible cells across */
static int s_gh = WORLD_W;
static int32_t s_cam_x, s_cam_y;    /* 8.8 world coords of the view centre */
static int s_ox, s_oy;              /* world coords of the top-left cell */
static int s_focus = -1;            /* koi the camera is following, or -1 */

#define CAM_LEAD 5                  /* world pixels ahead of the focus koi */

/// Light stays in 0..255; blit quantises it into LEVELS steps.
#define LIGHT_MAX 255

static inline void light_add(int vx, int vy, int amount)
{
    int v = s_light[vy][vx] + amount;
    if (v < 0) v = 0;
    if (v > LIGHT_MAX) v = LIGHT_MAX;
    s_light[vy][vx] = (int16_t)v;
}

static void camera_apply_zoom(void)
{
    s_pix = s_pix_min * ZOOM_NUM[s_zoom] / 4;
    s_gw = (s_rc.width + s_pix - 1) / s_pix;
    s_gh = (s_rc.height + s_pix - 1) / s_pix;
}

static void camera_update(void)
{
    int32_t tx = (int32_t)WORLD_CX << 8;
    int32_t ty = (int32_t)WORLD_CY << 8;

    if (s_focus >= 0 && s_focus < s_koi_count) {
        /* aim a little ahead of the fish: the easing below lags behind, and
         * the two roughly cancel out to keep it framed */
        const koi_t *k = &s_koi[s_focus];
        tx = k->x + COS(k->heading) * CAM_LEAD;
        ty = k->y + SIN(k->heading) * CAM_LEAD;
    }

    /* keep the window from wandering far past the rim into dead water */
    int off_max = POND_R + 8 - s_gw / 2;
    if (off_max < 0)
        off_max = 0;
    int dx = (int)((tx >> 8) - WORLD_CX);
    int dy = (int)((ty >> 8) - WORLD_CY);
    int d2 = dx * dx + dy * dy;
    if (d2 > off_max * off_max) {
        int d = isqrt32(d2);
        if (d > 0) {
            tx = ((int32_t)(WORLD_CX + dx * off_max / d)) << 8;
            ty = ((int32_t)(WORLD_CY + dy * off_max / d)) << 8;
        }
    }

    /* a lazy camera operator: always drifting towards the target */
    s_cam_x += (tx - s_cam_x) >> 4;
    s_cam_y += (ty - s_cam_y) >> 4;

    s_ox = (int)(s_cam_x >> 8) - s_gw / 2;
    s_oy = (int)(s_cam_y >> 8) - s_gh / 2;
}

// --- Water ---

/* What the hours in HOURS[] are tuned around: the light on open water at
 * the centre of the pond. The live value comes from the look. */
#define AMBIENT_CORE 120

static void draw_water(void)
{
    int a1 = (int)s_frame;
    int a2 = -2 * (int)s_frame;
    int a3 = 3 * (int)s_frame;
    const int r2max = POND_R * POND_R;
    const int amb = s_look.ambient;
    const int swell = s_look.swell;

    for (int vy = 0; vy < s_gh; vy++) {
        int y = s_oy + vy;
        int dy = y - WORLD_CY;
        int dy2 = dy * dy;
        uint8_t *mrow = s_mat[vy];
        int16_t *lrow = s_light[vy];
        for (int vx = 0; vx < s_gw; vx++) {
            int x = s_ox + vx;
            int dx = x - WORLD_CX;
            int d2 = dx * dx + dy2;

            /* slow crossing swells, quantised later into dithered bands */
            int w = SIN(x * 6 + y * 3 + a1)
                  + SIN(x * 3 - y * 7 + a2)
                  + (SIN(x * 11 + y * 9 + a3) >> 1);

            /* light drops off towards the rim: the pond edge is nearly black,
             * with a slight lean towards the upper left */
            int l = amb - (amb * d2) / r2max + (((w >> 4) * swell) >> 7)
                  - (dx + dy) / 3;
            if (l < 0) l = 0;

            mrow[vx] = MAT_WATER;
            lrow[vx] = (int16_t)l;
        }
    }
}

// --- Ripples ---

static void ripple_spawn(int wx, int wy, int delay, int life)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (s_ripples[i].active)
            continue;
        s_ripples[i].x = (int16_t)wx;
        s_ripples[i].y = (int16_t)wy;
        s_ripples[i].age = (int16_t)-delay;
        s_ripples[i].life = (int16_t)life;
        s_ripples[i].active = true;
        return;
    }
}

static void ripples_update(void)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!s_ripples[i].active)
            continue;
        if (++s_ripples[i].age >= s_ripples[i].life)
            s_ripples[i].active = false;
    }
}

/// One ring per ripple: a lit crest with a dark trough trailing behind it.
/// Kept subtle: a suggestion of a ring on the water, not a flash.
static void ripples_draw(void)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        const ripple_t *rp = &s_ripples[i];
        if (!rp->active || rp->age <= 0)
            continue;

        int rad = (rp->age * RIPPLE_SPEED) >> 8;
        if (rad < 1)
            continue;

        /* fade as the ring spreads out */
        int rem = rp->life - rp->age;
        int str = 3;
        if (rem < rp->life / 3)          str = 1;
        else if (rem < rp->life * 2 / 3) str = 2;

        int cx = rp->x - s_ox, cy = rp->y - s_oy;
        int lo = rad - 4;
        if (lo < 0)
            lo = 0;
        int hi = rad + 4;
        int lo2 = lo * lo, hi2 = hi * hi, r2 = rad * rad;
        int den = 2 * rad;

        int y0 = cy - hi, y1 = cy + hi;
        if (y0 < 0) y0 = 0;
        if (y1 >= s_gh) y1 = s_gh - 1;

        for (int vy = y0; vy <= y1; vy++) {
            int dy = vy - cy;
            int dy2 = dy * dy;
            if (dy2 > hi2)
                continue;
            int half = isqrt32(hi2 - dy2);
            int x0 = cx - half, x1 = cx + half;
            if (x0 < 0) x0 = 0;
            if (x1 >= s_gw) x1 = s_gw - 1;

            const uint8_t *mrow = s_mat[vy];
            for (int vx = x0; vx <= x1; vx++) {
                if (mrow[vx] != MAT_WATER)
                    continue;
                int dx = vx - cx;
                int d2 = dx * dx + dy2;
                if (d2 < lo2)
                    continue;

                /* (d2 - r2) / 2r approximates the signed distance to the ring */
                int k = (d2 - r2) / den;
                if (k >= -1 && k <= 1)
                    light_add(vx, vy, str * 16);
                else if (k == -2 || k == 2)
                    light_add(vx, vy, str * 6);
                else if (k > 2 && str >= 2)
                    light_add(vx, vy, -10);
            }
        }
    }
}

// --- Lily pads ---

static void pad_place(pad_t *p, int placed)
{
    p->r = (uint8_t)(6 + rnd(4));
    p->notch = (uint8_t)rnd(256);
    p->phase = (uint8_t)rnd(256);
    p->heading = (uint8_t)rnd(256);
    p->speed = (int16_t)(3 + rnd(5));      /* a very lazy drift */
    p->flower = (p->r >= 7) && (rnd(3) == 0);

    for (int tries = 0; tries < 80; tries++) {
        int reach = POND_R - p->r - 3;
        int a = (int)rnd(256);
        /* pick the radius by area so the pads do not bunch up in the middle */
        int d = isqrt32((int32_t)rnd((uint32_t)(reach * reach) + 1));
        int cx = WORLD_CX + ((COS(a) * d) >> 8);
        int cy = WORLD_CY + ((SIN(a) * d) >> 8);
        p->x = (int32_t)cx << 8;
        p->y = (int32_t)cy << 8;

        bool clear = true;
        for (int j = 0; j < placed; j++) {
            int dx = cx - (int)(s_pads[j].x >> 8);
            int dy = cy - (int)(s_pads[j].y >> 8);
            int min = p->r + s_pads[j].r + 4;
            if (dx * dx + dy * dy < min * min) {
                clear = false;
                break;
            }
        }
        if (clear)
            return;
    }
}

/// Pads float, so they drift, nudge each other apart and turn back when they
/// reach the far bank.
static void pad_update(pad_t *p, int idx)
{
    p->phase = (uint8_t)(p->phase + 2);
    if ((s_frame & 7) == 0)
        p->notch++;

    if (rnd(120) == 0)
        p->heading = (uint8_t)(p->heading + (int)rnd(41) - 20);

    /* Left to itself a drifting leaf is a 2D random walk, and a random walk
     * spends most of its time far from where it started, and over a few minutes
     * every pad ends up stranded against the rim. So the further out a pad
     * gets, the harder the pond turns it back: free in the middle, firmly
     * steered at the edge. */
    int wx = (int)(p->x >> 8), wy = (int)(p->y >> 8);
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    const int home = POND_R * 2 / 3;
    const int home2 = home * home;
    int d2 = dx * dx + dy * dy;
    if (d2 > home2) {
        int pull = (64 * (d2 - home2)) / (PAD_ROAM * PAD_ROAM - home2);
        if (pull > 64)
            pull = 64;
        int inward = angle_of(-dx, -dy);
        int diff = (int8_t)((uint8_t)inward - p->heading);
        int step = pull / 24 + 1;                  /* 1..3 units per frame */
        if (diff > step)  diff = step;
        if (diff < -step) diff = -step;
        p->heading = (uint8_t)(p->heading + diff);
    }

    /* leaves crowd but do not stack: push gently off any close neighbour */
    for (int j = 0; j < s_pad_count; j++) {
        if (j == idx)
            continue;
        int ox = wx - (int)(s_pads[j].x >> 8);
        int oy = wy - (int)(s_pads[j].y >> 8);
        int min = p->r + s_pads[j].r + 2;
        int d2 = ox * ox + oy * oy;
        if (d2 == 0 || d2 >= min * min)
            continue;
        int d = isqrt32(d2);
        if (d == 0)
            d = 1;
        p->x += (ox * 20) / d;
        p->y += (oy * 20) / d;
    }

    int speed = (p->speed * s_look.drift) >> 7;
    p->x += (COS(p->heading) * speed) >> 8;
    p->y += (SIN(p->heading) * speed) >> 8;
}

/// Pads read as dark silhouettes with a lit rim on the side facing the light.
static void draw_pad(const pad_t *p)
{
    int wx = (int)(p->x >> 8), wy = (int)(p->y >> 8);
    int fade = rim_fade(wx, wy);
    int cx = wx - s_ox;
    int cy = wy - s_oy + (SIN(p->phase) >> 8);   /* gentle bob, +/- 1 px */
    int r = p->r;
    int r2 = r * r;
    int inner2 = (r - 1) * (r - 1);
    int ncs = COS(p->notch), nsn = SIN(p->notch);
    int rim_lift = (90 * fade) >> 8;

    for (int dy = -r; dy <= r; dy++) {
        int vy = cy + dy;
        if (vy < 0 || vy >= s_gh)
            continue;
        for (int dx = -r; dx <= r; dx++) {
            int vx = cx + dx;
            if (vx < 0 || vx >= s_gw)
                continue;
            int d2 = dx * dx + dy * dy;
            if (d2 > r2)
                continue;

            /* narrow wedge notch, cut in from the rim but not to the centre */
            int dot = (dx * ncs + dy * nsn) >> 8;
            int cross = (-dx * nsn + dy * ncs) >> 8;
            if (dot > r / 4 && abs(cross) * 5 < (dot - r / 4) * 2)
                continue;

            int lit = s_light[vy][vx];
            if (d2 >= inner2 && dx + dy < 0) {
                s_mat[vy][vx] = MAT_PAD_RIM;         /* catches the light */
                lit = lit + rim_lift;
            } else {
                s_mat[vy][vx] = MAT_PAD;             /* in its own shadow */
                lit = (lit * 3) / 4 - 10 - (dx + dy) * 2;
            }
            if (lit < 0) lit = 0;
            if (lit > LIGHT_MAX) lit = LIGHT_MAX;
            s_light[vy][vx] = (int16_t)lit;
        }
    }

    if (!p->flower)
        return;

    /* a bright heart with four dim petals on the diagonals */
    static const int8_t PETALS[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        int vx = cx + PETALS[i][0], vy = cy + PETALS[i][1];
        if (vx < 0 || vx >= s_gw || vy < 0 || vy >= s_gh)
            continue;
        s_mat[vy][vx] = MAT_FLOWER;
        s_light[vy][vx] = (int16_t)((165 * fade) >> 8);
    }
    if (cx >= 0 && cx < s_gw && cy >= 0 && cy < s_gh) {
        s_mat[cy][cx] = MAT_FLOWER;
        s_light[cy][cx] = (int16_t)((LIGHT_MAX * fade) >> 8);
    }
}

// --- Koi ---

/**
 * Give one koi its own markings.
 *
 * Koi are not striped. A pattern is a handful of plates (large, irregular
 * groupings of colour) spread along the whole length of the fish rather
 * than bunched at one end, wandering off the spine instead of mirroring it.
 * A lone stray pixel of colour is the fault called tobi hi, so any marking
 * that ends up by itself is rubbed out again.
 */
static void koi_pattern(koi_t *k)
{
    memcpy(k->skin, s_koi_sprite, sizeof k->skin);

    /* a few come out plain, the way a single-colour ogon does */
    if (rnd(6) == 0)
        return;

    int plates = 2 + (int)rnd(3);
    int span = (KOI_W - 1) - KOI_BODY_X0;

    for (int p = 0; p < plates; p++) {
        /* one plate per band, so the markings run the length of the fish */
        int bx0 = KOI_BODY_X0 + (span * p) / plates;
        int bx1 = KOI_BODY_X0 + (span * (p + 1)) / plates;
        if (bx1 <= bx0)
            continue;

        int cx = bx0 + (int)rnd((uint32_t)(bx1 - bx0));
        int cy = KOI_H / 2 + (int)rnd(3) - 1;      /* off the spine, either way */
        int rx = 1 + (int)rnd(2);
        int ry = 1 + (int)rnd(2);
        int r2 = rx * rx * ry * ry;

        for (int y = cy - ry; y <= cy + ry; y++) {
            if (y < 0 || y >= KOI_H)
                continue;
            for (int x = cx - rx; x <= cx + rx; x++) {
                if (x < 0 || x >= KOI_W)
                    continue;
                if (k->skin[y][x] != 1)            /* body only, never a fin */
                    continue;
                int dx = x - cx, dy = y - cy;
                /* jittered edge, so a plate is never a clean ellipse */
                if (dx * dx * ry * ry + dy * dy * rx * rx <= r2 + (int)rnd(3) - 1)
                    k->skin[y][x] = 2;
            }
        }
    }

    /* no tobi hi: a marking with nothing next to it is noise, not a plate */
    for (int y = 0; y < KOI_H; y++) {
        for (int x = 0; x < KOI_W; x++) {
            if (k->skin[y][x] != 2)
                continue;
            bool joined = (y > 0          && k->skin[y - 1][x] == 2)
                       || (y < KOI_H - 1  && k->skin[y + 1][x] == 2)
                       || (x > 0          && k->skin[y][x - 1] == 2)
                       || (x < KOI_W - 1  && k->skin[y][x + 1] == 2);
            if (!joined)
                k->skin[y][x] = 1;
        }
    }
}

static void koi_reset(koi_t *k, int i)
{
    int a = (int)rnd(256);
    int d = (int)rnd(POND_R - 12);
    k->x = (int32_t)(WORLD_CX + ((COS(a) * d) >> 8)) << 8;
    k->y = (int32_t)(WORLD_CY + ((SIN(a) * d) >> 8)) << 8;
    k->heading = (uint8_t)rnd(256);
    k->phase = (uint8_t)rnd(256);
    k->speed = (int16_t)(70 + rnd(40));
    k->turn = 0;
    k->type = (uint8_t)(i % 3);
    k->boost = 0;
    koi_pattern(k);
}

static void koi_update(koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    int desired = -1;

    /* they are allowed out past the rim, they just turn back eventually */
    if (dx * dx + dy * dy > KOI_ROAM * KOI_ROAM) {
        desired = angle_of(-dx, -dy);
    } else if (k->boost) {
        int tdx = k->tx - wx, tdy = k->ty - wy;
        if (abs(tdx) + abs(tdy) < 5)
            k->boost = 0;
        else
            desired = angle_of(tdx, tdy);
    }

    if (desired >= 0) {
        int diff = (int8_t)((uint8_t)desired - k->heading);
        k->turn = (int8_t)(diff > 4 ? 4 : (diff < -4 ? -4 : diff));
    } else if (rnd(24) == 0) {
        k->turn = (int8_t)((int)rnd(5) - 2);
    }
    k->heading = (uint8_t)(k->heading + k->turn);

    int speed = k->speed;
    if (k->boost) {
        speed += speed / 2;
        k->boost--;
    }

    k->x += (COS(k->heading) * speed) >> 8;
    k->y += (SIN(k->heading) * speed) >> 8;
    k->phase = (uint8_t)(k->phase + 10 + (speed >> 4));
}

/// Soft halo the koi casts on the water around it.
static void draw_koi_glow(const koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int amount = (48 * rim_fade(wx, wy)) >> 8;
    if (amount <= 0)
        return;

    int cx = wx - s_ox, cy = wy - s_oy;
    const int r = KOI_GLOW_R;
    const int r2 = r * r;

    for (int vy = cy - r; vy <= cy + r; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy, dy2 = dy * dy;
        if (dy2 > r2)
            continue;
        const uint8_t *mrow = s_mat[vy];
        for (int vx = cx - r; vx <= cx + r; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            if (mrow[vx] != MAT_WATER)
                continue;
            int dx = vx - cx;
            int d2 = dx * dx + dy2;
            if (d2 > r2)
                continue;
            light_add(vx, vy, (amount * (r2 - d2)) / r2);
        }
    }
}

/// A koi under a lily pad still shows as light bleeding through the leaf,
/// so the fish never vanishes completely when you are zoomed in on it.
static void draw_koi_underglow(const koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int amount = (72 * rim_fade(wx, wy)) >> 8;
    if (amount <= 0)
        return;

    int cx = wx - s_ox, cy = wy - s_oy;
    const int r = 9;
    const int r2 = r * r;

    for (int vy = cy - r; vy <= cy + r; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy, dy2 = dy * dy;
        if (dy2 > r2)
            continue;
        const uint8_t *mrow = s_mat[vy];
        for (int vx = cx - r; vx <= cx + r; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            if (mrow[vx] != MAT_PAD)
                continue;
            int dx = vx - cx;
            int d2 = dx * dx + dy2;
            if (d2 > r2)
                continue;
            light_add(vx, vy, (amount * (r2 - d2)) / r2);
        }
    }
}

static void draw_koi(const koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int fade = rim_fade(wx, wy);
    int body_lit = 30 + ((200 * fade) >> 8);
    int tail_lit = 20 + ((128 * fade) >> 8);

    int cx = wx - s_ox, cy = wy - s_oy;
    int cs = COS(k->heading), sn = SIN(k->heading);
    const uint8_t *mats = KOI_MAT[k->type];
    int wig_amp = SIN(k->phase);

    for (int vy = cy - KOI_REACH; vy <= cy + KOI_REACH; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy;
        for (int vx = cx - KOI_REACH; vx <= cx + KOI_REACH; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            int dx = vx - cx;

            int bx = ((dx * cs + dy * sn) >> 8) + KOI_PIVOT;
            int by = ((-dx * sn + dy * cs) >> 8) + KOI_H / 2;
            if (bx < 0 || bx >= KOI_W)
                continue;

            /* the tail flicks, the head barely moves; the swing grows by
             * well under a pixel per column so the tail never tears away */
            if (bx < 10)
                by -= (wig_amp * (10 - bx)) >> 10;
            if (by < 0 || by >= KOI_H)
                continue;

            uint8_t v = k->skin[by][bx];
            if (!v)
                continue;

            s_mat[vy][vx] = mats[v - 1];
            /* the body is what glows; the tail is thinner and half sunk */
            s_light[vy][vx] = (int16_t)((v == 3) ? tail_lit : body_lit);
        }
    }
}

/// Index of the koi nearest the camera, for the zoom to latch onto.
static int koi_nearest_camera(void)
{
    int best = 0;
    int32_t best_d2 = INT32_MAX;
    for (int i = 0; i < s_koi_count; i++) {
        int32_t dx = (s_koi[i].x - s_cam_x) >> 8;
        int32_t dy = (s_koi[i].y - s_cam_y) >> 8;
        int32_t d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

// --- Drifting motes ---

static void mote_reset(mote_t *m)
{
    int a = (int)rnd(256);
    int d = (int)rnd(POND_R - 6);
    m->x = (int32_t)(WORLD_CX + ((COS(a) * d) >> 8)) << 8;
    m->y = (int32_t)(WORLD_CY + ((SIN(a) * d) >> 8)) << 8;
    m->heading = (uint8_t)rnd(256);
    m->phase = (uint8_t)rnd(256);
    m->speed = (int16_t)(10 + rnd(14));
}

static void mote_update(mote_t *m)
{
    m->phase = (uint8_t)(m->phase + 3);
    m->heading = (uint8_t)(m->heading + (SIN(m->phase) >> 6));

    int wx = (int)(m->x >> 8), wy = (int)(m->y >> 8);
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    if (dx * dx + dy * dy > MOTE_ROAM * MOTE_ROAM)
        m->heading = angle_of(-dx, -dy);

    int speed = (m->speed * s_look.drift) >> 7;
    m->x += (COS(m->heading) * speed) >> 8;
    m->y += (SIN(m->heading) * speed) >> 8;
}

static void draw_mote(const mote_t *m)
{
    int wx = (int)(m->x >> 8), wy = (int)(m->y >> 8);
    int fade = rim_fade(wx, wy);
    int cx = wx - s_ox, cy = wy - s_oy;
    if (cx < 0 || cx >= s_gw || cy < 0 || cy >= s_gh || fade <= 0)
        return;

    /* pulse, so the motes breathe rather than sit there; how hard they
     * burn is the look's business: barely there at noon, fireflies at
     * night, and a mist full of them */
    const int gain = s_look.mote_gain;
    int halo = (((26 + (SIN(m->phase * 2) >> 4)) * fade) >> 8) * gain >> 7;

    for (int vy = cy - 2; vy <= cy + 2; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy;
        for (int vx = cx - 2; vx <= cx + 2; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            int d2 = (vx - cx) * (vx - cx) + dy * dy;
            if (d2 > 4 || s_mat[vy][vx] != MAT_WATER)
                continue;
            light_add(vx, vy, halo - d2 * 5);
        }
    }

    int core = ((((190 + (SIN(m->phase * 2) >> 2))) * fade) >> 8) * gain >> 7;
    if (core > LIGHT_MAX)
        core = LIGHT_MAX;
    s_mat[cy][cx] = MAT_MOTE;
    s_light[cy][cx] = (int16_t)core;
}

// --- Scene: which look is on, and when it changes ---

/// Retarget the crossfade at whatever the pond should be showing now,
/// starting from what is on screen this instant, so a sky that changes
/// mid-sunrise carries on from where the sunrise had got to.
static void look_retarget(int frames)
{
    s_look_from = s_look;
    if (s_recording) {
        look_compose(&s_look_to, &REC_HOUR, &WEATHER[SKY_CLEAR]);
        memcpy(s_look_to.pigment, REC_PIGMENT, sizeof s_look_to.pigment);
    } else {
        look_compose(&s_look_to, &HOURS[s_hour], &WEATHER[s_sky]);
    }
    s_fade_at = 0;
    s_fade_frames = frames;

    /* a ring out from wherever you are looking, so the change reads as
     * something happening to the pond rather than a palette swap */
    ripple_spawn((int)(s_cam_x >> 8), (int)(s_cam_y >> 8), 0, RIPPLE_LIFE);
}

/// Roll the next sky. Weighted towards clear, with one rule: rain clears
/// through overcast rather than stopping dead or going on all day.
static int sky_pick(void)
{
    if (s_sky == SKY_RAIN)
        return rnd(2) ? SKY_OVERCAST : SKY_CLEAR;

    int total = 0;
    for (int i = 0; i < SKY_COUNT; i++)
        total += SKY_WEIGHT[i];

    int r = (int)rnd((uint32_t)total);
    for (int i = 0; i < SKY_COUNT; i++) {
        r -= SKY_WEIGHT[i];
        if (r < 0)
            return i;
    }
    return SKY_CLEAR;
}

static void scene_advance(void)
{
    s_hour = (s_hour + 1) % HOUR_COUNT;

    /* Fast forward walks the skies in order instead of rolling for them.
     * Six hours against four skies come back around together every twelve
     * scenes, so a hold long enough gets you all of both, and a demo never
     * waits on a dice roll to show you the rain. */
    s_sky = s_timelapse ? (s_sky + 1) % SKY_COUNT : sky_pick();

    look_retarget(s_timelapse ? FADE_FAST_FRAMES : FADE_SKY_FRAMES);
    plog("Scene: %s, %s", HOURS[s_hour].name, WEATHER[s_sky].name);
}

/// How long the scene on screen holds before the day moves on.
static int64_t scene_dwell(void)
{
    return s_timelapse ? TIMELAPSE_DWELL_US : DWELL_US;
}

/// Hold the current scene for its dwell, then move the day on. Recording
/// holds the clock: the pond's day waits while it is listening, and picks
/// up with a full dwell in hand.
static void scene_tick(void)
{
    int64_t now = s_now_us;
    int64_t dwell = scene_dwell();

    if (s_scene_due == 0)
        s_scene_due = now + dwell;
    if (dwell == 0 || s_recording) {
        s_scene_due = now + dwell;
        return;
    }
    if (now < s_scene_due)
        return;

    s_scene_due = now + dwell;
    scene_advance();
}

/// Rain is a lot of small rings, which the ripples already do for free.
/// The rings are kept short-lived so they stay small and read as drops
/// rather than as something falling in, and one in five is heard as well as
/// seen: a patter, not a downpour on a tin roof.
static void rain_step(void)
{
    if (!s_look.rain || (int)rnd(256) >= s_look.rain)
        return;

    int a = (int)rnd(256);
    int d = isqrt32((int32_t)rnd((uint32_t)(POND_R * POND_R)));
    ripple_spawn(WORLD_CX + ((COS(a) * d) >> 8),
                 WORLD_CY + ((SIN(a) * d) >> 8), 0, RIPPLE_LIFE / 5);

    if (rnd(5) == 0)
        emit(POND_SOUND_RAIN);
}

/// What can be heard in the scene besides the water. The rates live in the
/// look and so they crossfade with it: the birds of a dawn thin out as the
/// morning comes up, and the crickets of a dusk are already starting before
/// the light has finished going. Nothing here is heard while recording, the
/// sound module drops the lot.
static void ambience_step(void)
{
    if (s_look.birds && rnd(2048) < s_look.birds)
        emit(POND_SOUND_BIRD);
    if (s_look.insects && rnd(2048) < s_look.insects)
        emit(POND_SOUND_INSECT);
    if (s_look.frogs && rnd(2048) < s_look.frogs)
        emit(POND_SOUND_FROG);
    if (s_look.wind && rnd(2048) < s_look.wind)
        emit(POND_SOUND_WIND);
}

// --- Frame ---

/* Getting the cells onto the glass, s_pix screen pixels each. The zoom
 * steps do not all divide the panel evenly, so the last block of a row or
 * column is clipped; it falls outside the round bezel anyway. */

/* A band is `rows` screen rows of `w` pixels each, starting at screen
 * column `x0`, packed. The panel wants columns in fours, so the span is
 * widened to that before anything is drawn. */
typedef struct {
    uint16_t *buf;
    int x0, w;        /* screen columns covered */
    int y0, rows;     /* screen rows covered */
} band_t;

static void band_flush(band_t *b)
{
    s_rc.end_band(s_rc.ctx, b->x0, b->y0, b->w, b->rows, b->buf);
    s_flushed_px += (uint64_t)b->w * (uint64_t)b->rows;
    b->rows = 0;
}

static void blit(void)
{
    const int pix = s_pix;
    const bool all = !s_prev_valid || s_pal_dirty;

    s_drawn_frames++;

    /* Pass one: which cells changed. Each visible cell is quantised to a
     * palette index and compared with what it was; the changed span of
     * every row is kept, in cells. Nothing is drawn yet. */
    static int16_t cx0[WORLD_W], cx1[WORLD_W];
    for (int vy = 0; vy < s_gh; vy++) {
        const uint8_t *mrow = s_mat[vy];
        const int16_t *lrow = s_light[vy];
        const int8_t *drow = s_dither[vy & 3];
        uint8_t *prow = s_prev_idx[vy];

        cx0[vy] = -1;
        for (int vx = 0; vx < s_gw; vx++) {
            int l = lrow[vx] + drow[vx & 3];
            int level = (l * LEVELS) >> 8;
            if (level < 0) level = 0;
            if (level >= LEVELS) level = LEVELS - 1;

            uint8_t idx = (uint8_t)(mrow[vx] * LEVELS + level);
            if (all || prow[vx] != idx) {
                prow[vx] = idx;
                if (cx0[vy] < 0) cx0[vy] = (int16_t)vx;
                cx1[vy] = (int16_t)vx;
                s_dirty_cells++;
            }
        }
        s_seen_cells += (uint32_t)s_gw;
    }

    /* Pass two: runs of changed rows become bands, each at most band_rows
     * screen rows tall (band_rows) and as wide as the union of its rows'
     * spans, in fours because panels want it so. A band is expanded from
     * the palette indices straight into its packed place in the buffer the
     * host hands out, one row per world row and then copied down `pix`
     * times. */
    for (int vy = 0; vy < s_gh;) {
        if (cx0[vy] < 0) {
            vy++;
            continue;
        }
        int y0 = vy * pix;
        int end = vy;
        int px0 = cx0[vy], px1 = cx1[vy];
        while (end + 1 < s_gh && cx0[end + 1] >= 0
               && (end + 2 - vy) * pix <= s_rc.band_rows) {
            end++;
            if (cx0[end] < px0) px0 = cx0[end];
            if (cx1[end] > px1) px1 = cx1[end];
        }
        px0 = (px0 * pix) & ~3;
        px1 = ((px1 + 1) * pix + 3) & ~3;
        if (px1 > s_rc.width) px1 = s_rc.width;
        int w = px1 - px0;
        int rows_total = (end + 1) * pix;
        if (rows_total > s_rc.height) rows_total = s_rc.height;
        rows_total -= y0;

        band_t band = { .buf = s_rc.begin_band(s_rc.ctx), .x0 = px0, .w = w,
                        .y0 = y0, .rows = rows_total };

        uint16_t *dst = band.buf;
        int rows_left = rows_total;
        for (int r = vy; r <= end && rows_left > 0; r++) {
            const uint8_t *prow = s_prev_idx[r];
            /* one packed row: cells overlapping [px0, px1) */
            int x = px0;
            for (int vx = px0 / pix; x < px1; vx++) {
                uint16_t c = s_pal[prow[vx]];
                int cell_end = (vx + 1) * pix;
                if (cell_end > px1) cell_end = px1;
                while (x < cell_end) {
                    dst[x - px0] = c;
                    x++;
                }
            }
            /* and the same row again for the rest of the world row */
            int rows = rows_left < pix ? rows_left : pix;
            for (int k = 1; k < rows; k++)
                memcpy(dst + (size_t)k * w, dst, (size_t)w * sizeof(uint16_t));
            dst += (size_t)rows * w;
            rows_left -= rows;
        }
        band_flush(&band);
        vy = end + 1;
    }

    s_prev_valid = true;
    s_pal_dirty = false;
}

/// One extra pass of world motion, with nothing drawn: what fast forward
/// spends its extra time on. Only the things that move. Rain, the scene
/// clock and everything that makes a noise stay at one pass a frame, so
/// running fast never turns the ambience into a stutter of birds.
static void world_motion(void)
{
    for (int i = 0; i < s_koi_count; i++)
        koi_update(&s_koi[i]);
    ripples_update();
    for (int i = 0; i < s_pad_count; i++)
        pad_update(&s_pads[i], i);
    for (int i = 0; i < s_mote_count; i++)
        mote_update(&s_motes[i]);
}

static void world_substep(void)
{
    s_frame++;
    world_motion();
}

/// One tick of the pond. The world always moves; it is only drawn when
/// `draw` is set, which the idle tier uses to halve what goes to the panel
/// without slowing the day or the fish down.
static void pond_step(bool draw)
{
    s_frame++;
    look_step();
    scene_tick();
    rain_step();
    ambience_step();

    if (s_koi_count > 0 && --s_next_surface == 0) {
        /* a koi nosing the surface somewhere */
        const koi_t *k = &s_koi[rnd((uint32_t)s_koi_count)];
        ripple_spawn((int)(k->x >> 8), (int)(k->y >> 8), 0, RIPPLE_LIFE / 2);
        emit(POND_SOUND_SURFACE);
        s_next_surface = (uint16_t)(90 + rnd(160));
    }

    if (--s_next_distant == 0) {
        /* something falling in over by the far bank */
        int a = (int)rnd(256);
        int d = POND_R - 6 - (int)rnd(8);
        ripple_spawn(WORLD_CX + ((COS(a) * d) >> 8),
                     WORLD_CY + ((SIN(a) * d) >> 8), 0, RIPPLE_LIFE * 2 / 3);
        emit(POND_SOUND_DISTANT);
        s_next_distant = (uint16_t)(220 + rnd(420));
    }

    if (s_timelapse)
        for (int i = 1; i < TIMELAPSE_STEPS; i++)
            world_substep();

    if (!draw) {
        world_motion();
        return;
    }

    camera_update();
    draw_water();

    for (int i = 0; i < s_koi_count; i++) {
        koi_update(&s_koi[i]);
        draw_koi_glow(&s_koi[i]);
    }

    ripples_update();
    ripples_draw();

    for (int i = 0; i < s_koi_count; i++)
        draw_koi(&s_koi[i]);

    for (int i = 0; i < s_pad_count; i++) {
        pad_update(&s_pads[i], i);
        draw_pad(&s_pads[i]);
    }
    for (int i = 0; i < s_koi_count; i++)
        draw_koi_underglow(&s_koi[i]);

    for (int i = 0; i < s_mote_count; i++) {
        mote_update(&s_motes[i]);
        draw_mote(&s_motes[i]);
    }

    blit();
}

void pond_tick(int64_t now_us, bool draw)
{
    s_now_us = now_us;
    pond_step(draw);
}

void pond_stats(pond_stats_t *out, bool reset)
{
    if (out) {
        out->frames_drawn = s_drawn_frames;
        out->cells_seen = s_seen_cells;
        out->cells_changed = s_dirty_cells;
        out->pixels_sent = s_flushed_px;
    }
    if (reset) {
        s_drawn_frames = 0;
        s_seen_cells = s_dirty_cells = 0;
        s_flushed_px = 0;
    }
}

// --- Setup ---

static void dither_init(void)
{
    /* 4x4 ordered dither, scaled to half a light step either side */
    static const uint8_t BAYER[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 },
    };
    const int step = 256 / LEVELS;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            s_dither[y][x] = (int8_t)((BAYER[y][x] * step) / 16 - step / 2);
}

static void sprite_init(void)
{
    for (int y = 0; y < KOI_H; y++) {
        for (int x = 0; x < KOI_W; x++) {
            switch (KOI_ART[y][x]) {
            case 'm': s_koi_sprite[y][x] = 1; break;
            case 'a': s_koi_sprite[y][x] = 2; break;
            case 'f': s_koi_sprite[y][x] = 3; break;
            default:  s_koi_sprite[y][x] = 0; break;
            }
        }
    }
}

static int clamp_count(int want, int max)
{
    if (want < 0) return 0;
    if (want > max) return max;
    return want;
}

// --- Public API ---

void pond_set_population(int koi, int pads, int motes)
{
    koi = clamp_count(koi, POND_MAX_KOI);
    pads = clamp_count(pads, POND_MAX_PADS);
    motes = clamp_count(motes, POND_MAX_MOTES);

    for (int i = s_koi_count; i < koi; i++)
        koi_reset(&s_koi[i], i);
    for (int i = s_pad_count; i < pads; i++)
        pad_place(&s_pads[i], i);
    for (int i = s_mote_count; i < motes; i++)
        mote_reset(&s_motes[i]);

    s_koi_count = koi;
    s_pad_count = pads;
    s_mote_count = motes;

    if (s_focus >= s_koi_count)
        s_focus = (s_zoom > 0 && s_koi_count > 0) ? koi_nearest_camera() : -1;
}

void pond_get_population(int *koi, int *pads, int *motes)
{
    if (koi)   *koi = s_koi_count;
    if (pads)  *pads = s_pad_count;
    if (motes) *motes = s_mote_count;
}

void pond_init(const pond_config_t *cfg, const pond_render_t *render)
{
    s_cfg = *cfg;
    s_rc = *render;
    if (s_rc.band_rows < 16)
        s_rc.band_rows = 16;
    if (cfg->seed)
        s_rng = cfg->seed;

    /* the smallest whole number of screen pixels per world pixel that
     * still gets the whole world onto the screen */
    s_pix_min = (s_rc.width + WORLD_W - 1) / WORLD_W;
    if (s_pix_min < 1)
        s_pix_min = 1;

    trig_init();
    dither_init();
    sprite_init();

    s_cam_x = (int32_t)WORLD_CX << 8;
    s_cam_y = (int32_t)WORLD_CY << 8;
    camera_apply_zoom();

    /* open somewhere random in the day, so waking the Watcher is not
     * always dawn. There is an RTC on the board but nothing sets it, so
     * this is the pond's own day, not the one outside the window. */
    s_hour = (int)rnd(HOUR_COUNT);
    s_sky = sky_pick();
    look_compose(&s_look, &HOURS[s_hour], &WEATHER[s_sky]);
    s_look_from = s_look_to = s_look;
    palette_build(&s_look);
    s_scene_due = 0;   /* set from the clock on the first tick */

    pond_set_population(cfg->koi, cfg->pads, cfg->motes);

    s_next_surface = (uint16_t)(90 + rnd(160));
    s_next_distant = (uint16_t)(220 + rnd(420));

    plog("Pond ready: %dx%d world, %d koi, %d pads, %d motes",
             WORLD_W, WORLD_W, s_koi_count, s_pad_count, s_mote_count);
    plog("Scene: %s, %s (holding %d s)", HOURS[s_hour].name,
         WEATHER[s_sky].name, s_cfg.dwell_sec);
}

void pond_tap(int x, int y)
{
    int wx = s_ox + x / s_pix;
    int wy = s_oy + y / s_pix;

    /* three staggered rings read as one spreading disturbance */
    ripple_spawn(wx, wy, 0, RIPPLE_LIFE);
    ripple_spawn(wx, wy, 5, RIPPLE_LIFE * 3 / 4);
    ripple_spawn(wx, wy, 11, RIPPLE_LIFE / 2);

    for (int i = 0; i < s_koi_count; i++) {
        s_koi[i].tx = (int16_t)wx;
        s_koi[i].ty = (int16_t)wy;
        s_koi[i].boost = (uint16_t)(70 + rnd(30));
    }
}

void pond_set_recording(bool on)
{
    if (on == s_recording)
        return;
    s_recording = on;
    look_retarget(FADE_REC_FRAMES);
}

void pond_set_timelapse(bool on)
{
    if (on == s_timelapse)
        return;
    s_timelapse = on;

    /* Move the day on the instant the hold takes, so the pond answers the
     * finger rather than sitting on the rest of a dwell it had already
     * started. Letting go just leaves the scene where the fast forward
     * left it, with a full dwell in hand. A recording holds the day still
     * whatever the finger is doing, so it does not get this one either. */
    if (on && !s_recording)
        scene_advance();

    s_scene_due = s_now_us + scene_dwell();
    plog("Fast forward %s", on ? "on" : "off");
}

const char *pond_hour_name(void)
{
    return s_recording ? REC_HOUR.name : HOURS[s_hour].name;
}

const char *pond_sky_name(void)
{
    return WEATHER[s_sky].name;
}

/* The backlight follows the light in the scene. Ambient runs from about 32
 * on a rainy night to 172 on a clear noon, and the glass need not be lit
 * any harder for a dark pond than for a bright one: on an IPS panel a dim
 * backlight makes the night blacker rather than greyer. 30% to 70%. */
int pond_backlight_percent(void)
{
    int pct = 30 + ((int)s_look.ambient * 40) / 172;
    if (pct < 30) pct = 30;
    if (pct > 70) pct = 70;
    return pct;
}

bool pond_zoom(int delta)
{
    int z = s_zoom + delta;
    if (z < 0) z = 0;
    if (z >= ZOOM_STEPS) z = ZOOM_STEPS - 1;
    /* on a small screen two detents can round to the same pixel size */
    int dir = delta > 0 ? 1 : -1;
    while (z != s_zoom && z + dir >= 0 && z + dir < ZOOM_STEPS
           && s_pix_min * ZOOM_NUM[z] / 4 == s_pix)
        z += dir;
    if (z == s_zoom || s_pix_min * ZOOM_NUM[z] / 4 == s_pix)
        return false;

    s_zoom = z;
    camera_apply_zoom();
    s_prev_valid = false;   /* every cell moved: repaint the lot */

    /* wide open the camera sits on the pond; any closer and it picks a koi
     * to drift after, so zooming in never lands on empty water */
    if (s_zoom == 0 || s_koi_count == 0)
        s_focus = -1;
    else if (s_focus < 0)
        s_focus = koi_nearest_camera();
    return true;
}
