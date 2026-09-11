#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "board.h"
#include "recorder.h"
#include "screen.h"
#include "sound.h"

static const char *TAG = "main";

#define INACTIVITY_TIMEOUT_MS (CONFIG_KOI_DEEP_SLEEP_TIMEOUT_SEC * 1000)

/* The press that wakes the Watcher can still be held when the button driver
 * comes up, and would then land as a click and start a recording nobody
 * asked for. Ignore clicks for the first moment after boot. */
#define WAKE_GUARD_MS 1200

/* Long enough for the recorder to drain its ring and close the file. */
#define FLUSH_TIMEOUT_MS 3000

static TimerHandle_t s_inactivity_timer;
static int64_t s_boot_us;

static void reset_inactivity_timer(void)
{
    xTimerReset(s_inactivity_timer, 0);
    screen_activity();
}

static void inactivity_timer_cb(TimerHandle_t t)
{
    (void)t;

    if (recorder_is_active()) {
        /* the pond is listening; nobody is idle */
        xTimerReset(s_inactivity_timer, 0);
        return;
    }

    ESP_LOGI(TAG, "Inactivity timeout, entering deep sleep");
    board_deep_sleep(0);
}

static void on_long_press(void)
{
    ESP_LOGI(TAG, "Button long press, entering deep sleep");
    recorder_flush(FLUSH_TIMEOUT_MS);   /* never sleep on an open file */
    board_deep_sleep(0);
}

/// A click of the knob button starts a recording, and the next one saves it.
static void on_click(void)
{
    if (esp_timer_get_time() - s_boot_us < (int64_t)WAKE_GUARD_MS * 1000)
        return;
    recorder_toggle();
    reset_inactivity_timer();
}

/// The pond wears the recording look for as long as there is a file open,
/// so the colour on the glass is the recording's own state and not a guess
/// at it.
static void on_recorder_state(bool recording)
{
    ESP_LOGI(TAG, "%s", recording ? "Recording started" : "Recording saved");
    screen_set_recording(recording);
}

static void on_knob(int dir)
{
    screen_knob(dir);
    reset_inactivity_timer();
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    s_boot_us = esp_timer_get_time();

    board_init();
    sound_init();
    recorder_set_state_cb(on_recorder_state);
    recorder_init();
    screen_init();

    s_inactivity_timer = xTimerCreate("inact", pdMS_TO_TICKS(INACTIVITY_TIMEOUT_MS),
                                       pdFALSE, NULL, inactivity_timer_cb);
    xTimerStart(s_inactivity_timer, 0);

    board_set_btn_press_cb(reset_inactivity_timer);
    board_set_btn_click_cb(on_click);
    board_set_btn_long_press_cb(on_long_press);
    board_set_knob_cb(on_knob);
    screen_set_tap_cb(reset_inactivity_timer);

    recorder_debug_start();   /* nothing unless CONFIG_KOI_DEBUG_RECORDER */
}
