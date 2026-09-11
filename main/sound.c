/**
 * Pond sound design, synthesised on the fly. There are no audio assets.
 *
 * The plop of something hitting water is not the drop itself. Phillips,
 * Agarwal and Jordan filmed it (Scientific Reports, 2018) and found the
 * sound is driven by a small air bubble trapped under the surface: the
 * impact makes a brief click, the crater takes a few milliseconds to form,
 * and then the entrapped bubble rings and drives the surface like a piston.
 * https://www.nature.com/articles/s41598-018-27913-0
 *
 * So each voice here is that same three-part event:
 *
 *   1. a short filtered noise click for the impact,
 *   2. a few milliseconds of nothing while the crater forms,
 *   3. a decaying sine at the bubble's resonance.
 *
 * The pitch comes from Minnaert's 1933 result that a bubble in water
 * resonates at f0 * r ~= 3.26 Hz*m, so voices are specified by bubble
 * radius rather than by frequency and the pitch follows from the physics.
 * https://en.wikipedia.org/wiki/Minnaert_resonance
 *
 * The pitch also rises as it rings, which is the part your ear reads as
 * "water". Van den Doel's liquid sound model (ACM TAP, 2005) captures it as
 * f(t) = f0 * (1 + XI * d * t) against an exp(-d * t) decay, with XI ~= 0.1
 * found experimentally. That works out to roughly a 3 * XI rise over the
 * audible life of the bubble whatever the damping, and it costs one add per
 * sample. https://dl.acm.org/doi/10.1145/1101530.1101554
 *
 * Voices are mixed in a single task and fed through two damped feedback
 * combs, which is just enough reverb to put the pond in a dark room. The
 * task only streams while something is sounding, so silence costs nothing.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "sound.h"

static const char *TAG = "sound";

#define SR            DRV_AUDIO_SAMPLE_RATE   /* 16 kHz, mono, 16-bit */
#define BLOCK         256                     /* 16 ms per write       */
#define MAX_VOICES    6      /* rain and crickets overlap freely */
#define MASTER_VOLUME 70

/* Minnaert: f0 * r ~= 3.26 Hz*m, so f0 = 3.26e6 / r for r in micrometres. */
#define MINNAERT 3260000.0f

/* Van den Doel's rise constant. 0.1 is the measured value; turning it up
 * exaggerates the "wet" chirp, which some tiny speakers need. */
#ifndef BUBBLE_XI
#define BUBBLE_XI 0.10f
#endif

/* Two feedback combs at 77 ms and 108 ms, damped in the loop. */
#define COMB_A 1231
#define COMB_B 1723
#define TAIL_BLOCKS (SR / BLOCK)              /* render ~1 s of tail out */

// --- Voice presets ---

typedef struct {
    uint16_t r_lo, r_hi;    /* entrapped bubble radius, micrometres        */
    uint16_t f_lo, f_hi;    /* tone in Hz, for the voices that are not
                               bubbles and have no radius to speak of      */
    int16_t  glide;         /* Hz per second. A bubble takes its rise from
                               van den Doel instead, so leave this at 0    */
    uint16_t damping;       /* d, per second                               */
    uint16_t len_ms;
    uint16_t click_ms;      /* noise decay                                 */
    uint16_t rise_ms;       /* fade in, for anything that should not start
                               with a bang                                 */
    uint8_t  amp;           /* tone level, 0 = noise only                  */
    uint8_t  send;          /* into the reverb, 0..255                     */
    uint8_t  click;         /* impact transient level, 0..255              */
    uint8_t  click_lp;      /* noise brightness: one-pole coeff, 0..255    */
    uint8_t  click_hp;      /* and the rumble taken back off underneath it */
    uint8_t  delay_ms;      /* crater forming, before the bubble rings     */
    uint8_t  trem_hz;       /* warble rate: what makes a cricket a cricket */
    uint8_t  trem_depth;    /* and how deep it warbles, 0..255             */
} preset_t;

/* 3000-4200 um is roughly 780-1090 Hz, about a fingertip's worth of trapped
 * air; the ambient voices use bigger, lazier bubbles. A real tap drip traps
 * a bubble ten times smaller and plinks near 9 kHz, which this 16 kHz codec
 * and its little speaker could not reproduce anyway.
 *
 * The last five are not water. A bird, a cricket and a frog have nothing to
 * do with Minnaert, so they are written as a frequency and a warble rather
 * than as a radius, and they say so by leaving r_lo at zero. They are the
 * same three-part voice underneath: a tone, an envelope, and a little noise
 * where the sound starts. */
static const preset_t PRESETS[SOUND_COUNT] = {
    [SOUND_DROP] = {
        .r_lo = 3000, .r_hi = 4200, .damping = 30, .len_ms = 200,
        .amp = 150, .send = 190, .click = 110, .click_ms = 4,
        .click_lp = 140, .delay_ms = 7,
    },
    [SOUND_SURFACE] = {
        .r_lo = 5000, .r_hi = 7000, .damping = 22, .len_ms = 280,
        .amp = 70, .send = 150, .click = 45, .click_ms = 6,
        .click_lp = 90, .delay_ms = 10,
    },
    [SOUND_DISTANT] = {
        .r_lo = 6000, .r_hi = 9000, .damping = 20, .len_ms = 280,
        .amp = 34, .send = 230, .click = 20, .click_ms = 7,
        .click_lp = 70, .delay_ms = 12,
    },
    [SOUND_TICK] = {
        .r_lo = 4000, .r_hi = 4000, .damping = 30, .len_ms = 40,
        .amp = 0, .send = 0, .click = 95, .click_ms = 3, .click_lp = 205,
    },
    /* A raindrop is a far smaller pocket of air than a fingertip, so by
     * Minnaert it rings a good deal higher and dies much faster. */
    [SOUND_RAIN] = {
        .r_lo = 1100, .r_hi = 1700, .damping = 60, .len_ms = 90,
        .amp = 62, .send = 60, .click = 38, .click_ms = 2,
        .click_lp = 210, .delay_ms = 2,
    },
    [SOUND_BIRD] = {
        .f_lo = 2500, .f_hi = 3500, .glide = 2400, .damping = 20,
        .len_ms = 110, .rise_ms = 8, .amp = 80, .send = 120,
    },
    [SOUND_INSECT] = {
        .f_lo = 4200, .f_hi = 4800, .damping = 5, .len_ms = 260,
        .rise_ms = 25, .amp = 40, .send = 70, .trem_hz = 30, .trem_depth = 235,
    },
    [SOUND_FROG] = {
        .f_lo = 170, .f_hi = 250, .damping = 7, .len_ms = 300,
        .rise_ms = 12, .amp = 115, .send = 110, .trem_hz = 19, .trem_depth = 215,
    },
    /* No tone at all: a gust is filtered noise that arrives and leaves
     * slowly, which is the only reason the voice needed a fade in. Low
     * passed on its own it comes out as a subsonic rumble, most of it under
     * the frequency this speaker can move at all, so the bottom is taken
     * back off and what is left sits where a small cone can actually
     * shift air. */
    [SOUND_WIND] = {
        .len_ms = 2200, .rise_ms = 600, .amp = 0, .send = 200,
        .click = 170, .click_ms = 500, .click_lp = 70, .click_hp = 12,
    },
};

// --- Oscillator table ---

#define LUT 512
static float s_sine[LUT + 1];

static inline float osc(float phase)
{
    float f = phase * LUT;
    int i = (int)f;
    return s_sine[i] + (s_sine[i + 1] - s_sine[i]) * (f - (float)i);
}

static inline float noise(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// --- State ---

typedef struct {
    bool active;
    uint32_t left;      /* samples remaining */
    uint32_t delay;     /* samples before the bubble starts ringing */
    float phase;        /* 0..1 */
    float freq, dfreq;  /* the rising resonance and its per-sample step */
    float env, env_k;
    float click, click_k, click_lp, lp;
    float click_hp, hp;
    float gain, gain_up; /* fade in, 1.0 for everything that starts at once */
    float trem_phase, trem_inc, trem_depth;
    uint32_t rng;
    float send;
} voice_t;

static voice_t s_voices[MAX_VOICES];

static float s_ca[COMB_A], s_cb[COMB_B];
static int s_ia, s_ib;
static float s_lpa, s_lpb;

static int16_t s_block[BLOCK];
static QueueHandle_t s_queue;
static atomic_bool s_muted;

/// Exponential per-sample multiplier that decays to 1/e in `ms`.
static float decay_ms_k(float ms)
{
    float samples = ms * (float)SR / 1000.0f;
    if (samples < 1.0f)
        samples = 1.0f;
    return expf(-1.0f / samples);
}

static void voice_start(sound_t which)
{
    if (which < 0 || which >= SOUND_COUNT)
        return;
    const preset_t *p = &PRESETS[which];

    /* steal the quietest voice if they are all busy */
    voice_t *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s_voices[i].active) {
            v = &s_voices[i];
            break;
        }
        if (!v || s_voices[i].env < v->env)
            v = &s_voices[i];
    }

    float d = (float)p->damping;
    float f0, glide;

    if (p->r_lo) {
        /* water: the pitch comes from the size of the trapped bubble, and
         * the rise with it */
        uint32_t r = p->r_lo;
        if (p->r_hi > p->r_lo)
            r += esp_random() % (uint32_t)(p->r_hi - p->r_lo + 1);
        f0 = MINNAERT / (float)r;
        glide = f0 * BUBBLE_XI * d;          /* f(t) = f0 (1 + XI d t) */
    } else {
        f0 = (float)p->f_lo;
        if (p->f_hi > p->f_lo)
            f0 += (float)(esp_random() % (uint32_t)(p->f_hi - p->f_lo + 1));
        glide = (float)p->glide;
    }

    v->active = true;
    v->left = (uint32_t)p->len_ms * SR / 1000;
    v->delay = (uint32_t)p->delay_ms * SR / 1000;
    v->phase = 0.0f;
    v->freq = f0;
    v->dfreq = glide / (float)SR;
    v->env = (float)p->amp / 255.0f;
    v->env_k = expf(-d / (float)SR);
    v->click = (float)p->click / 255.0f;
    v->click_k = decay_ms_k((float)p->click_ms);
    v->click_lp = (float)p->click_lp / 255.0f;
    v->click_hp = (float)p->click_hp / 255.0f;
    v->lp = 0.0f;
    v->hp = 0.0f;
    v->rng = esp_random() | 1u;
    v->send = (float)p->send / 255.0f;

    uint32_t rise = (uint32_t)p->rise_ms * SR / 1000;
    v->gain = rise ? 0.0f : 1.0f;
    v->gain_up = rise ? 1.0f / (float)rise : 0.0f;

    v->trem_phase = 0.0f;
    v->trem_inc = (float)p->trem_hz / (float)SR;
    v->trem_depth = (float)p->trem_depth / 255.0f;
}

static bool voices_active(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active)
            return true;
    return false;
}

static void reverb_reset(void)
{
    memset(s_ca, 0, sizeof s_ca);
    memset(s_cb, 0, sizeof s_cb);
    s_lpa = s_lpb = 0.0f;
    s_ia = s_ib = 0;
}

static void render_block(bool fade)
{
    for (int n = 0; n < BLOCK; n++) {
        float dry = 0.0f, wet = 0.0f;

        for (int i = 0; i < MAX_VOICES; i++) {
            voice_t *v = &s_voices[i];
            if (!v->active)
                continue;

            float s = 0.0f;

            /* the impact itself: a short, dull noise burst */
            if (v->click > 0.0005f) {
                v->lp += (noise(&v->rng) - v->lp) * v->click_lp;
                float nz = v->lp;
                if (v->click_hp > 0.0f) {
                    /* second, much slower pole, subtracted: what is left is
                     * a band rather than everything down to DC */
                    v->hp += (nz - v->hp) * v->click_hp;
                    nz -= v->hp;
                }
                s += nz * v->click;
                v->click *= v->click_k;
            }

            /* then, once the crater has formed, the trapped bubble rings */
            if (v->delay) {
                v->delay--;
            } else if (v->env > 0.0f) {
                v->phase += v->freq * (1.0f / (float)SR);
                if (v->phase >= 1.0f)
                    v->phase -= 1.0f;
                s += osc(v->phase) * v->env;
                v->freq += v->dfreq;      /* f(t) = f0 (1 + XI d t) */
                v->env *= v->env_k;
            }

            /* a warble deep enough to chop the tone into pulses is what
             * separates a cricket from a tuning fork */
            if (v->trem_depth > 0.0f) {
                v->trem_phase += v->trem_inc;
                if (v->trem_phase >= 1.0f)
                    v->trem_phase -= 1.0f;
                s *= 1.0f - v->trem_depth * (0.5f - 0.5f * osc(v->trem_phase));
            }

            if (v->gain < 1.0f) {
                v->gain += v->gain_up;
                if (v->gain > 1.0f)
                    v->gain = 1.0f;
                s *= v->gain;
            }

            dry += s;
            wet += s * v->send;
            if (--v->left == 0)
                v->active = false;
        }

        float ya = s_ca[s_ia], yb = s_cb[s_ib];
        s_lpa += (ya - s_lpa) * 0.45f;
        s_lpb += (yb - s_lpb) * 0.36f;
        s_ca[s_ia] = wet + s_lpa * 0.55f;
        s_cb[s_ib] = wet + s_lpb * 0.50f;
        if (++s_ia >= COMB_A) s_ia = 0;
        if (++s_ib >= COMB_B) s_ib = 0;

        float out = dry + (ya + yb) * 0.32f;
        if (fade)
            out *= (float)(BLOCK - n) / (float)BLOCK;

        /* soft limiter: near linear for one drop, bends instead of clipping
         * when several land at once */
        out = tanhf(out * 0.9f);

        s_block[n] = (int16_t)(out * 29000.0f);
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    bsp_codec_volume_set(MASTER_VOLUME, NULL);

    int tail = 0;
    for (;;) {
        sound_t kind;

        /* nothing sounding and the room has gone quiet: sleep until poked */
        if (!voices_active() && tail <= 0) {
            if (xQueueReceive(s_queue, &kind, portMAX_DELAY) != pdTRUE)
                continue;
            voice_start(kind);
        }
        while (xQueueReceive(s_queue, &kind, 0) == pdTRUE)
            voice_start(kind);

        bool fade = false;
        if (atomic_load(&s_muted)) {
            /* recording: drop what is sounding rather than hand the mic a
             * plop, and take the reverb tail with it */
            for (int i = 0; i < MAX_VOICES; i++)
                s_voices[i].active = false;
            if (tail > 0) {
                tail = 0;
                fade = true;
            }
        } else if (voices_active()) {
            tail = TAIL_BLOCKS;
        } else if (--tail <= 0) {
            tail = 0;
            fade = true;          /* last block, ramp out so it cannot click */
        }

        render_block(fade);

        size_t written;
        bsp_i2s_write(s_block, sizeof s_block, &written, 200);

        if (fade)
            reverb_reset();
    }
}

// --- Public API ---

void sound_init(void)
{
    if (s_queue)
        return;

    for (int i = 0; i <= LUT; i++)
        s_sine[i] = sinf((float)i * 2.0f * (float)M_PI / (float)LUT);

    s_queue = xQueueCreate(8, sizeof(sound_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create sound queue");
        return;
    }

    if (xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to start audio task");
}

void sound_play(sound_t which)
{
    if (!s_queue || atomic_load(&s_muted))
        return;
    xQueueSend(s_queue, &which, 0);
}

void sound_set_muted(bool muted)
{
    atomic_store(&s_muted, muted);
}
