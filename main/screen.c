#include <stdatomic.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "board.h"
#include "pond.h"
#include "pond_port.h"
#include "screen.h"
#include "sound.h"

static const char *TAG = "screen";

#define POLL_MS 50

/* Power tiers. Somebody at the glass gets the full pond; leave it alone
 * for IDLE_MS and the backlight eases down to IDLE_BACKLIGHT_PCT of what
 * the scene asked for. Anything you do to it brings it straight back. */
#define IDLE_MS            20000
#define IDLE_BACKLIGHT_PCT 50
#define IDLE_DRAW_EVERY    2      /* one frame drawn in every two ticks */
#define BACKLIGHT_STEP     2      /* percent per poll, so a change is a fade */

static void (*s_tap_cb)(void);

static atomic_llong s_last_activity_us;
static bool s_recording;
static int s_backlight = -1;      /* what the LEDC is actually set to */
static bool s_lit;                /* first frame has gone out; backlight allowed */

static void note_activity(void)
{
    atomic_store(&s_last_activity_us, esp_timer_get_time());
}

static bool is_idle(void)
{
    if (s_recording)
        return false;   /* somebody is listening, the glass should show it */
    return esp_timer_get_time() - atomic_load(&s_last_activity_us)
           > (int64_t)IDLE_MS * 1000;
}

/// Ease the backlight towards what the scene and the power tier ask for.
static void backlight_step(void)
{
    if (!s_lit)
        return;
    static bool was_idle;
    bool idle = is_idle();
    if (idle != was_idle) {
        was_idle = idle;
        pond_port_set_draw_every(idle ? IDLE_DRAW_EVERY : 1);
        ESP_LOGI(TAG, "%s", idle ? "Idle: backlight easing down, half the frames"
                                 : "Active: backlight back up, every frame");
    }

    int target = pond_backlight_percent();
    if (idle)
        target = target * IDLE_BACKLIGHT_PCT / 100;

    int next = s_backlight;
    if (next < 0)
        next = target;                     /* first time: no fade from black */
    else if (next < target)
        next = next + BACKLIGHT_STEP > target ? target : next + BACKLIGHT_STEP;
    else if (next > target)
        next = next - BACKLIGHT_STEP < target ? target : next - BACKLIGHT_STEP;

    if (next != s_backlight) {
        s_backlight = next;
        board_set_lcd_brightness(next);
    }
}

/// Knob events and the recorder both arrive on other tasks, so they are
/// only parked here and picked up from the LVGL task, which owns the pond's
/// state. -1 in the recording request means nothing is waiting.
static atomic_int s_knob_detents;
static atomic_int s_rec_req = -1;

/// Give LVGL a frame or two to push the first pond render out to the panel
/// before the backlight comes up, so the display never flashes garbage.
static void backlight_cb(lv_timer_t *t)
{
    s_lit = true;
    backlight_step();
    lv_timer_del(t);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    int detents = atomic_exchange(&s_knob_detents, 0);
    if (detents != 0 && pond_zoom(detents))
        sound_play(SOUND_TICK);

    int rec = atomic_exchange(&s_rec_req, -1);
    if (rec >= 0) {
        s_recording = rec != 0;
        pond_set_recording(s_recording);
    }

    backlight_step();
}

static void screen_press_cb(lv_event_t *e)
{
    (void)e;

    lv_point_t p = { DRV_LCD_H_RES / 2, DRV_LCD_V_RES / 2 };
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
        lv_indev_get_point(indev, &p);

    sound_play(SOUND_DROP);
    pond_tap(p.x, p.y);
    note_activity();

    if (s_tap_cb)
        s_tap_cb();
}

/// A finger left on the glass runs the pond's day fast, and lifting it puts
/// the day back to its own pace. LVGL calls the press a long one after its
/// long-press time, so a tap is still a tap and only a hold counts.
static void screen_hold_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    bool held = code == LV_EVENT_LONG_PRESSED ||
                code == LV_EVENT_LONG_PRESSED_REPEAT;

    pond_set_timelapse(held);
    note_activity();

    /* A finger on the glass is somebody there, the same as a tap is, and
     * the repeat is the only word we get while it stays down: without it a
     * long enough hold would idle the Watcher into deep sleep underneath
     * the demo it is running. */
    if (s_tap_cb)
        s_tap_cb();
}

/// LVGL still runs the touch panel and the knob, and it still renders its
/// (empty) screen into its own buffers when asked, but nothing it renders
/// goes to the glass. The pond flushes itself; see blit() in pond.c.
static void flush_nothing(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    (void)area; (void)color_p;
    lv_disp_flush_ready(drv);
}

void screen_init(void)
{
    note_activity();
    lvgl_port_lock(0);

    lv_disp_t *disp = lv_disp_get_default();
    disp->driver->flush_cb = flush_nothing;
    while (disp->driver->draw_buf->flushing)   /* let a flush already queued land */
        vTaskDelay(1);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_hold_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(scr, screen_hold_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, screen_hold_cb, LV_EVENT_PRESS_LOST, NULL);

    pond_port_init();
    lv_timer_create(backlight_cb, 200, NULL);
    lv_timer_create(poll_cb, POLL_MS, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Screen ready");
}

void screen_set_tap_cb(void (*cb)(void))
{
    s_tap_cb = cb;
}

void screen_knob(int dir)
{
    atomic_fetch_add(&s_knob_detents, dir);
    note_activity();
}

void screen_activity(void)
{
    note_activity();
}

void screen_set_recording(bool on)
{
    atomic_store(&s_rec_req, on ? 1 : 0);
}
