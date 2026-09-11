#include <assert.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_io_expander_pca95xx_16bit.h"
#include "sensecap-watcher.h"
#include "board.h"

static const char *TAG = "board";

static SemaphoreHandle_t s_codec_mutex;
static int s_codec_users;

#if CONFIG_MOCHI_DEBUG_RECORDER
#define CODEC_LOG(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define CODEC_LOG(...) ESP_LOGD(TAG, __VA_ARGS__)
#endif

/**
 * Recover the touch I2C bus. bsp_i2c_bus_init() configures the touch I2C pins
 * (GPIO 38/39) as outputs driven low, which can leave the SPD2010 touch
 * controller in a stuck state. Toggle SCL 9 times followed by a STOP condition
 * to force any stuck slave to release the bus.
 */
static void touch_i2c_bus_recover(void)
{
    gpio_set_direction(BSP_TOUCH_I2C_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BSP_TOUCH_I2C_SDA, GPIO_PULLUP_ONLY);
    gpio_set_direction(BSP_TOUCH_I2C_SCL, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(BSP_TOUCH_I2C_SCL, GPIO_PULLUP_ONLY);

    for (int i = 0; i < 9; i++) {
        gpio_set_level(BSP_TOUCH_I2C_SCL, 1);
        esp_rom_delay_us(5);
        gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
        esp_rom_delay_us(5);
    }

    /* STOP condition: SDA transitions low-to-high while SCL is high */
    gpio_set_direction(BSP_TOUCH_I2C_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_TOUCH_I2C_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 1);
    esp_rom_delay_us(5);
}

/**
 * Park the AI chip's chip-select high.
 *
 * The SD card (GPIO 46) and the Himax vision chip (GPIO 21) share SPI2, one
 * CS each. The BSP drives the card's CS high before it talks to the vision
 * chip, but nothing does the reverse. On a board like this one, which
 * never brings the vision chip up, its CS is left floating with the chip
 * powered. The card's own clock edges then select it, the Himax drives MISO
 * against the card, and every read comes back with a CRC error. Park it high
 * once at boot and the card has the bus to itself.
 */
static void ai_chip_deselect(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_SSCMA_CLIENT_SPI_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(BSP_SSCMA_CLIENT_SPI_CS, 1);
}

void board_init(void)
{
    ESP_LOGI(TAG, "Initializing board");

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wake cause: %d (0=reset, 2=ext0, 4=timer)", (int)cause);

    // IO expander first: restores power rails after deep sleep
    bsp_io_expander_init();
    ai_chip_deselect();
    ESP_ERROR_CHECK(bsp_codec_init());

    // Recover touch I2C bus before LVGL tries to talk to the SPD2010
    touch_i2c_bus_recover();
    vTaskDelay(pdMS_TO_TICKS(200));

    lv_disp_t *disp = bsp_lvgl_init();
    assert(disp);

    /* The BSP brings every rail up at boot. This firmware never talks to
     * the Himax vision chip or its camera, the Grove socket or the battery
     * ADC divider, and the card is only wanted while recording, so those
     * four go straight back off. The recorder raises the card's own rail
     * for as long as it has a file open. */
    bsp_exp_io_set_level(BSP_PWR_AI_CHIP | BSP_PWR_GROVE | BSP_PWR_BAT_ADC
                         | BSP_PWR_SDCARD, 0);
    ESP_LOGI(TAG, "Unused rails off: AI chip, Grove, battery ADC, SD card");

    /* The codec came up open, streaming silence with the amplifier on.
     * Nothing wants it yet: the first sound, or the first recording, takes
     * it through board_codec_acquire(). */
    s_codec_mutex = xSemaphoreCreateMutex();
    bsp_codec_dev_stop();
    bsp_exp_io_set_level(BSP_PWR_CODEC_PA, 0);
    /* six lines of codec chatter per plop is a lot of plops */
    esp_log_level_set("I2S_IF", ESP_LOG_WARN);
    esp_log_level_set("Adev_Codec", ESP_LOG_WARN);

    /* Run flat out while any task is running, and drop to 80 MHz whenever
     * both cores are idle, which after a frame is most of the time. APB
     * stays at 80 MHz either way, so no peripheral notices. */
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
    ESP_LOGI(TAG, "DFS on: %d to %d MHz", pm.min_freq_mhz, pm.max_freq_mhz);

    ESP_LOGI(TAG, "Board init done");
}

// --- Codec power ---

/* The sound task and the recorder each hold the codec for as long as they
 * need it, and it is powered for as long as anybody holds it. Between a
 * plop and the next one the ES8311 is suspended, the I2S clocks are
 * stopped and the amplifier rail is off. */
void board_codec_acquire(void)
{
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    if (s_codec_users++ == 0) {
        bsp_exp_io_set_level(BSP_PWR_CODEC_PA, 1);

        /* The same opens the BSP's resume does, minus the closes it puts
         * in front of them: closing a closed device only logs an error.
         * Both devices open together because they share one ES8311 and
         * closing either one suspends the chip under the other. */
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = DRV_AUDIO_SAMPLE_RATE,
            .channel = DRV_AUDIO_CHANNELS,
            .bits_per_sample = DRV_AUDIO_SAMPLE_BITS,
        };
        /* open() disables the I2S channel before it reclocks it, whether or
         * not it was running, and the driver logs an error for the second
         * case. Ours always is the second case. */
        esp_log_level_set("i2s_common", ESP_LOG_NONE);
        esp_codec_dev_set_in_gain(bsp_codec_microphone_get(), DRV_AUDIO_MIC_GAIN);
        esp_codec_dev_open(bsp_codec_speaker_get(), &fs);
        fs.channel = 2;
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        esp_codec_dev_open(bsp_codec_microphone_get(), &fs);
        esp_log_level_set("i2s_common", ESP_LOG_INFO);
        CODEC_LOG("Codec on: amplifier rail up, ES8311 open");
    }
    xSemaphoreGive(s_codec_mutex);
}

void board_codec_release(void)
{
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    if (s_codec_users > 0 && --s_codec_users == 0) {
        bsp_codec_dev_stop();
        bsp_exp_io_set_level(BSP_PWR_CODEC_PA, 0);
        CODEC_LOG("Codec off: ES8311 suspended, amplifier rail down");
    }
    xSemaphoreGive(s_codec_mutex);
}

void board_sdcard_power(bool on)
{
    bsp_exp_io_set_level(BSP_PWR_SDCARD, on ? 1 : 0);
    if (on)
        vTaskDelay(pdMS_TO_TICKS(50));   /* let the card come up before the first command */
}

// --- Button callbacks ---

static lv_indev_t *find_encoder(void)
{
    lv_indev_t *indev = NULL;
    while (1) {
        indev = lv_indev_get_next(indev);
        if (indev == NULL || indev->driver->type == LV_INDEV_TYPE_ENCODER)
            break;
    }
    return indev;
}

static void btn_cb_wrapper(void *arg, void *arg2)
{
    void (*cb)(void) = arg2;
    if (cb) cb();
}

void board_set_btn_press_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_DOWN, btn_cb_wrapper, cb);
}

void board_set_btn_release_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_UP, btn_cb_wrapper, cb);
}

void board_set_btn_long_press_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_LONG_PRESS_UP, btn_cb_wrapper, cb);
}

void board_set_btn_click_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_SINGLE_CLICK, btn_cb_wrapper, cb);
}

// --- Knob rotation ---

static void (*s_knob_cb)(int dir);

static void knob_left_cb(void *arg, void *arg2)
{
    (void)arg; (void)arg2;
    if (s_knob_cb) s_knob_cb(-1);
}

static void knob_right_cb(void *arg, void *arg2)
{
    (void)arg; (void)arg2;
    if (s_knob_cb) s_knob_cb(1);
}

void board_set_knob_cb(void (*cb)(int dir))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    s_knob_cb = cb;
    lvgl_port_encoder_register_event_cb(enc, KNOB_LEFT, knob_left_cb, NULL);
    lvgl_port_encoder_register_event_cb(enc, KNOB_RIGHT, knob_right_cb, NULL);
}

// --- Deep sleep ---

/// Read PCA9535 input register over I2C to clear the interrupt latch.
static void io_expander_clear_interrupt(void)
{
    uint8_t reg = 0x00;
    uint8_t buf[2];
    esp_err_t err = i2c_master_write_read_device(BSP_GENERAL_I2C_NUM,
        ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001,
        &reg, 1, buf, 2, pdMS_TO_TICKS(100));
    ESP_LOGD(TAG, "IO exp clear INT: err=%d, port0=0x%02x, port1=0x%02x",
             err, buf[0], buf[1]);
}

void board_deep_sleep(uint32_t time_sec)
{
    ESP_LOGI(TAG, "Deep sleep requested (timer=%lus)", (unsigned long)time_sec);

    esp_io_expander_handle_t io_exp = bsp_io_expander_init();

    // Power off peripherals
    uint32_t pin_mask_sleep = BSP_PWR_SDCARD | BSP_PWR_CODEC_PA | BSP_PWR_GROVE
                            | BSP_PWR_BAT_ADC | BSP_PWR_LCD | BSP_PWR_AI_CHIP;
    bsp_exp_io_set_level(pin_mask_sleep, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Make floating pins into outputs to prevent spurious interrupts.
    // Keep only button + charge/VBUS/standby as inputs.
    uint32_t keep_inputs = BSP_PWR_CHRG_DET | BSP_PWR_STDBY_DET
                         | BSP_PWR_VBUS_IN_DET | BSP_KNOB_BTN;
    uint32_t pins_to_output = DRV_IO_EXP_INPUT_MASK & ~keep_inputs;
    esp_io_expander_set_dir(io_exp, pins_to_output, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_exp, pins_to_output, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Clear pending IO expander interrupt
    for (int i = 0; i < 10; i++) {
        io_expander_clear_interrupt();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BSP_IO_EXPANDER_INT) == 1)
            break;
    }

    if (time_sec > 0)
        esp_sleep_enable_timer_wakeup((uint64_t)time_sec * 1000000);

    esp_sleep_enable_ext0_wakeup(BSP_IO_EXPANDER_INT, 0);
    rtc_gpio_pullup_en(BSP_IO_EXPANDER_INT);
    rtc_gpio_pulldown_dis(BSP_IO_EXPANDER_INT);

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}

void board_set_lcd_brightness(int percent)
{
    bsp_lcd_brightness_set(percent);
}
