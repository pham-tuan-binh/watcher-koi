/**
 * The pond on the SenseCAP Watcher.
 *
 * pond.c knows nothing about this board. This file gives it what it asks
 * for through pond_render_t and not much else:
 *
 *   pixels   Two small band buffers in internal memory, because the SPI
 *            driver cannot DMA out of PSRAM and would copy anything there
 *            into internal memory itself, once per flush. A band is sent
 *            with esp_lcd_panel_draw_bitmap() and the panel keeps whatever
 *            is not resent. begin_band() blocks until at most one band is
 *            still in flight, so the buffer it hands out is never the one
 *            the DMA is reading; the transfer-done interrupt wakes it.
 *   clock    esp_timer.
 *   random   esp_random(), once, for the seed.
 *   sound    sound_play().
 *   log      ESP_LOGI.
 *
 * LVGL still owns the touch panel and the knob but no longer draws to the
 * glass; screen_init() points its flush at nothing.
 */

#include <stdatomic.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "pond.h"
#include "pond_port.h"
#include "sound.h"

static const char *TAG = "pond";

#define BAND_ROWS 40                  /* screen rows per staging buffer */
#define FRAME_LOG_EVERY 750           /* ticks between log lines, 30 s */

static uint16_t *s_stage[2];
static int s_stage_i;
static esp_lcd_panel_handle_t s_panel;
static TaskHandle_t s_draw_task;      /* whoever is inside pond_tick() */
static atomic_int s_inflight;         /* bands the DMA has not finished */

static int s_draw_every = 1;
static int s_draw_tick;

// --- Pixels ---

/// The SPI transfer of a band has finished. From the SPI driver's interrupt.
static bool IRAM_ATTR flush_done_cb(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    atomic_fetch_sub(&s_inflight, 1);
    BaseType_t woken = pdFALSE;
    TaskHandle_t t = s_draw_task;
    if (t)
        vTaskNotifyGiveFromISR(t, &woken);
    return woken == pdTRUE;
}

/// Transfers finish in order, so once at most one is in flight the buffer
/// used two bands ago is free again.
static uint16_t *begin_band(void *ctx)
{
    (void)ctx;
    while (atomic_load(&s_inflight) > 1)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    return s_stage[s_stage_i];
}

static void end_band(void *ctx, int x, int y, int w, int h, const uint16_t *px)
{
    (void)ctx;
    atomic_fetch_add(&s_inflight, 1);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, px);
    if (err != ESP_OK) {
        atomic_fetch_sub(&s_inflight, 1);
        ESP_LOGE(TAG, "Band flush failed: %s", esp_err_to_name(err));
    }
    s_stage_i ^= 1;
}

// --- Sound and log ---

static void play(void *ctx, pond_sound_t which)
{
    (void)ctx;
    static const sound_t MAP[POND_SOUND_COUNT] = {
        [POND_SOUND_DROP] = SOUND_DROP,       [POND_SOUND_SURFACE] = SOUND_SURFACE,
        [POND_SOUND_DISTANT] = SOUND_DISTANT, [POND_SOUND_RAIN] = SOUND_RAIN,
        [POND_SOUND_BIRD] = SOUND_BIRD,       [POND_SOUND_INSECT] = SOUND_INSECT,
        [POND_SOUND_FROG] = SOUND_FROG,       [POND_SOUND_WIND] = SOUND_WIND,
    };
    if (which >= 0 && which < POND_SOUND_COUNT)
        sound_play(MAP[which]);
}

static void log_line(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

// --- Tick ---

/* Frame timing, logged every FRAME_LOG_EVERY ticks: how long a tick takes,
 * how often one happens, and what the pond says it drew. */
static int64_t s_frame_prev_us, s_frame_step_us, s_frame_period_us;
static int s_frame_n;

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    bool draw = ++s_draw_tick >= s_draw_every;
    if (draw)
        s_draw_tick = 0;

    s_draw_task = xTaskGetCurrentTaskHandle();
    int64_t t0 = esp_timer_get_time();
    pond_tick(t0, draw);
    int64_t t1 = esp_timer_get_time();

    s_frame_step_us += t1 - t0;
    if (s_frame_prev_us)
        s_frame_period_us += t0 - s_frame_prev_us;
    s_frame_prev_us = t0;
    if (++s_frame_n == FRAME_LOG_EVERY) {
        pond_stats_t st;
        pond_stats(&st, true);
        uint64_t screen = (uint64_t)DRV_LCD_H_RES * DRV_LCD_V_RES;
        ESP_LOGI(TAG, "Frames: tick %lld us, period %lld us (%d fps), "
                 "drawn 1 in %d, %u%% of cells changed, %u%% of pixels sent",
                 (long long)(s_frame_step_us / s_frame_n),
                 (long long)(s_frame_period_us / (s_frame_n - 1)),
                 (int)(1000000LL * (s_frame_n - 1) / s_frame_period_us),
                 s_draw_every,
                 st.cells_seen ? (unsigned)(100ULL * st.cells_changed / st.cells_seen) : 0u,
                 st.frames_drawn ? (unsigned)(100ULL * st.pixels_sent
                                              / (st.frames_drawn * screen)) : 0u);
        s_frame_n = 0;
        s_frame_step_us = 0;
        s_frame_period_us = 0;
    }
}

void pond_port_set_draw_every(int n)
{
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    s_draw_every = n;
}

void pond_port_init(void)
{
    size_t stage_size = (size_t)DRV_LCD_H_RES * BAND_ROWS * sizeof(uint16_t);
    for (int i = 0; i < 2; i++) {
        s_stage[i] = heap_caps_aligned_alloc(64, stage_size,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_stage[i]) {
            ESP_LOGE(TAG, "Failed to allocate %u byte staging buffer", (unsigned)stage_size);
            return;
        }
    }
    ESP_LOGI(TAG, "Staging: 2 x %u bytes internal, %u bytes internal left",
             (unsigned)stage_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    s_panel = bsp_lcd_get_panel_handle();
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_done_cb };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
        bsp_lcd_get_panel_io_handle(), &cbs, NULL));

    const pond_render_t render = {
        .width = DRV_LCD_H_RES,
        .height = DRV_LCD_V_RES,
        .band_rows = BAND_ROWS,
        .swap_bytes = true,           /* CONFIG_LV_COLOR_16_SWAP: what the SPD2010 takes */
        .begin_band = begin_band,
        .end_band = end_band,
        .sound = play,
        .log = log_line,
    };
    const pond_config_t cfg = {
        .koi = CONFIG_MOCHI_POND_KOI_COUNT,
        .pads = CONFIG_MOCHI_POND_LILY_COUNT,
        .motes = CONFIG_MOCHI_POND_MOTE_COUNT,
        .dwell_sec = CONFIG_MOCHI_SCENE_DWELL_SEC,
        .seed = esp_random(),
    };
    pond_init(&cfg, &render);

    /* have something on screen before the backlight comes up */
    s_draw_task = xTaskGetCurrentTaskHandle();
    pond_tick(esp_timer_get_time(), true);
    lv_timer_create(frame_cb, POND_TICK_MS, NULL);
}
