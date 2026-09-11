/**
 * The recorder's self-test, for CONFIG_KOI_DEBUG_RECORDER builds.
 *
 * Nothing here needs a finger on the Watcher. Two seconds after boot it
 * puts the card through its paces, then records on its own and checks
 * what it wrote, all of it to the serial log:
 *
 *   1. detect, power, mount; free space; what is in /POND
 *   2. write a test file, read it back, compare, time both; delete it
 *   3. unmount and power down
 *   4. record for KOI_DEBUG_REC_SEC through the normal path
 *   5. reopen the WAV: header against file size, then the samples for
 *      peak, RMS and DC, which is how you tell a working mic from a
 *      recording of zeros
 */

#include "sdkconfig.h"

#if CONFIG_KOI_DEBUG_RECORDER

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "recorder.h"

static const char *TAG = "rec_debug";

#define REC_DIR   DRV_BASE_PATH_SD "/POND"
#define TEST_FILE REC_DIR "/SELFTEST.BIN"
#define TEST_KB   64
#define BLOCK     4096

static uint8_t s_block[BLOCK];

static void list_dir(void)
{
    DIR *d = opendir(REC_DIR);
    if (!d) {
        ESP_LOGI(TAG, "%s does not exist yet", REC_DIR);
        return;
    }
    unsigned files = 0;
    unsigned long long bytes = 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[sizeof REC_DIR + 260];
        snprintf(path, sizeof path, REC_DIR "/%s", e->d_name);
        struct stat st;
        long size = stat(path, &st) == 0 ? (long)st.st_size : -1;
        ESP_LOGI(TAG, "  %-14s %ld bytes", e->d_name, size);
        files++;
        if (size > 0)
            bytes += (unsigned long long)size;
    }
    closedir(d);
    ESP_LOGI(TAG, "%s: %u files, %llu bytes", REC_DIR, files, bytes);
}

static void card_test(void)
{
    ESP_LOGI(TAG, "=== Card ===");
    if (!recorder_card_open()) {
        ESP_LOGE(TAG, "No card, or it would not mount: skipping the card test");
        return;
    }

    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(DRV_BASE_PATH_SD, &total, &free_bytes) == ESP_OK)
        ESP_LOGI(TAG, "Filesystem: %llu MB, %llu MB free",
                 (unsigned long long)(total >> 20), (unsigned long long)(free_bytes >> 20));
    list_dir();

    mkdir(REC_DIR, 0777);
    FILE *f = fopen(TEST_FILE, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Could not create %s", TEST_FILE);
        goto out;
    }
    for (int i = 0; i < BLOCK; i++)
        s_block[i] = (uint8_t)(i * 7 + 3);
    int64_t t0 = esp_timer_get_time();
    bool ok = true;
    for (int k = 0; k < TEST_KB * 1024 / BLOCK && ok; k++) {
        s_block[0] = (uint8_t)k;           /* so blocks are not identical */
        ok = fwrite(s_block, 1, BLOCK, f) == BLOCK;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    int64_t t1 = esp_timer_get_time();
    if (!ok) {
        ESP_LOGE(TAG, "Write of %s failed", TEST_FILE);
        goto out;
    }
    ESP_LOGI(TAG, "Wrote %d KB in %d ms (%d KB/s)", TEST_KB, (int)((t1 - t0) / 1000),
             (int)((int64_t)TEST_KB * 1000000 / (t1 - t0)));

    f = fopen(TEST_FILE, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Could not reopen %s", TEST_FILE);
        goto out;
    }
    t0 = esp_timer_get_time();
    unsigned bad = 0;
    for (int k = 0; k < TEST_KB * 1024 / BLOCK; k++) {
        if (fread(s_block, 1, BLOCK, f) != BLOCK) {
            ESP_LOGE(TAG, "Short read at block %d", k);
            bad++;
            break;
        }
        if (s_block[0] != (uint8_t)k)
            bad++;
        for (int i = 1; i < BLOCK; i++)
            if (s_block[i] != (uint8_t)(i * 7 + 3)) {
                bad++;
                break;
            }
    }
    fclose(f);
    t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "Read %d KB back in %d ms (%d KB/s), %u bad blocks%s", TEST_KB,
             (int)((t1 - t0) / 1000), (int)((int64_t)TEST_KB * 1000000 / (t1 - t0)),
             bad, bad ? "  <-- FAIL" : "");
    unlink(TEST_FILE);

out:
    recorder_card_close();
}

static void check_wav(const char *path)
{
    ESP_LOGI(TAG, "=== Checking %s ===", path);
    if (!recorder_card_open()) {
        ESP_LOGE(TAG, "Card gone before the check");
        return;
    }

    struct stat st;
    FILE *f = fopen(path, "rb");
    if (!f || stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Could not open %s", path);
        goto out;
    }

    uint8_t h[44];
    if (fread(h, 1, sizeof h, f) != sizeof h) {
        ESP_LOGE(TAG, "File is shorter than a WAV header");
        goto out;
    }
    uint32_t riff_size = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    uint32_t rate = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
    uint16_t channels = (uint16_t)(h[22] | h[23] << 8);
    uint16_t bits = (uint16_t)(h[34] | h[35] << 8);
    uint32_t data_size = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
    bool tags = memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0
             && memcmp(h + 36, "data", 4) == 0;
    bool sizes = riff_size == (uint32_t)st.st_size - 8
              && data_size == (uint32_t)st.st_size - 44;
    ESP_LOGI(TAG, "Header: tags %s, %lu Hz, %u ch, %u bit, riff %lu data %lu, file %ld bytes: %s",
             tags ? "ok" : "BAD", (unsigned long)rate, channels, bits,
             (unsigned long)riff_size, (unsigned long)data_size, (long)st.st_size,
             tags && sizes ? "ok" : "MISMATCH  <-- FAIL");

    /* the samples: a mic that is not really connected records zeros, a
     * codec that came up wrong records a rail, and both have no RMS */
    int16_t *smp = (int16_t *)s_block;
    uint32_t n = 0, zeros = 0;
    int peak = 0;
    int64_t sum = 0, sum2 = 0;
    size_t got;
    while ((got = fread(s_block, 1, sizeof s_block, f)) >= 2) {
        for (size_t i = 0; i < got / 2; i++) {
            int v = smp[i];
            if (v == 0) zeros++;
            if (abs(v) > peak) peak = abs(v);
            sum += v;
            sum2 += (int64_t)v * v;
            n++;
        }
    }
    if (n == 0) {
        ESP_LOGE(TAG, "No samples in the file  <-- FAIL");
        goto out;
    }
    int dc = (int)(sum / (int64_t)n);
    int rms = (int)sqrt((double)(sum2 / (int64_t)n));
    ESP_LOGI(TAG, "Samples: %lu (%lu.%lu s), peak %d, rms %d, dc %d, %lu%% zero: %s",
             (unsigned long)n, (unsigned long)(n / rate), (unsigned long)(n % rate * 10 / rate),
             peak, rms, dc, (unsigned long)(100ULL * zeros / n),
             rms < 20 ? "SILENT  <-- suspicious" : "signal present");

out:
    if (f)
        fclose(f);
    recorder_card_close();
}

static void debug_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));   /* let the pond come up first */

    card_test();

    ESP_LOGI(TAG, "=== Recording %d s ===", CONFIG_KOI_DEBUG_REC_SEC);
    recorder_toggle();
    vTaskDelay(pdMS_TO_TICKS(CONFIG_KOI_DEBUG_REC_SEC * 1000));
    recorder_flush(5000);
    vTaskDelay(pdMS_TO_TICKS(200));    /* let the log lines land in order */

    const char *path = recorder_last_path();
    if (path[0] == '\0')
        ESP_LOGE(TAG, "No recording was saved  <-- FAIL");
    else
        check_wav(path);

    ESP_LOGI(TAG, "=== Self-test done ===");
    vTaskDelete(NULL);
}

void recorder_debug_start(void)
{
    if (xTaskCreate(debug_task, "rec_debug", 6144, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to start the self-test task");
}

#else

void recorder_debug_start(void)
{
}

#endif
