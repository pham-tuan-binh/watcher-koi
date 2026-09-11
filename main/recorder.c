/**
 * Recording the room the pond sits in, onto the SD card.
 *
 * The mic gives 16 kHz mono, which is 32 KB/s, nothing at all for a card.
 * The catch is latency, not throughput: the card is on SPI2 and a FAT write
 * can stall for tens of milliseconds while a cluster gets allocated, while
 * the I2S input only has about 90 ms of DMA sitting behind it. One task
 * that read a chunk and then wrote it would drop samples on any card having
 * a bad day.
 *
 * So there are two tasks with a ring between them: a reader that does
 * nothing but pull from the mic, and a writer that empties whatever has
 * piled up into the file. The ring is four seconds deep in PSRAM, which is
 * far more stall than a card should ever ask for.
 *
 * The speaker is muted for the length of a recording. The Watcher's mic and
 * its speaker are millimetres apart, so a plop played while recording is a
 * plop *in* the recording. A pond that falls silent the moment it starts
 * listening says what is going on better than any icon could.
 *
 * WAV wants its two sizes in a header at the front of the file, and neither
 * is known until the end, so the header goes down as zeros and is patched
 * on the way out.
 */

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "recorder.h"
#include "sound.h"

static const char *TAG = "recorder";

#define REC_DIR     DRV_BASE_PATH_SD "/POND"
#define REC_SR      DRV_AUDIO_SAMPLE_RATE
#define REC_BYTES_PER_SEC (REC_SR * 2)

#define CHUNK_BYTES (1024)             /* 32 ms of mic per read       */
#define RING_BYTES  (128 * 1024)       /* ~4 s of card stall absorbed */
#define WRITE_BYTES (8 * 1024)         /* the biggest bite of the ring
                                          the writer takes at once     */
#define MAX_BYTES   ((size_t)CONFIG_MOCHI_REC_MAX_SEC * REC_BYTES_PER_SEC)

/* Long enough to cover a mic read already in flight when we stopped. */
#define DRAIN_MS    60

// --- WAV ---

typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t riff_size;      /* everything in the file after this field */
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t format;         /* 1 = uncompressed PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char     data[4];
    uint32_t data_size;
} wav_hdr_t;

static wav_hdr_t wav_header(uint32_t data_bytes)
{
    wav_hdr_t h = {
        .riff = { 'R', 'I', 'F', 'F' },
        .riff_size = (uint32_t)(sizeof(wav_hdr_t) - 8) + data_bytes,
        .wave = { 'W', 'A', 'V', 'E' },
        .fmt = { 'f', 'm', 't', ' ' },
        .fmt_size = 16,
        .format = 1,
        .channels = 1,
        .sample_rate = REC_SR,
        .byte_rate = REC_BYTES_PER_SEC,
        .block_align = 2,
        .bits = 16,
        .data = { 'd', 'a', 't', 'a' },
        .data_size = data_bytes,
    };
    return h;
}

// --- State ---

static StreamBufferHandle_t s_ring;
static SemaphoreHandle_t s_done;      /* given each time a file is closed */
static TaskHandle_t s_writer_task;
static TaskHandle_t s_reader_task;

static atomic_bool s_want;            /* what the button last asked for */
static atomic_bool s_pulling;         /* whether the reader should be reading */

static void (*s_state_cb)(bool recording);

static bool s_mounted;
static FILE *s_file;
static char s_path[32];
static uint32_t s_dropped;

static uint8_t s_chunk[CHUNK_BYTES];
static uint8_t s_buf[WRITE_BYTES];

// --- Card and file ---

/// Mount on demand rather than at boot, so a card pushed in later works and
/// a pond with no card in it costs nothing.
static bool sd_ready(void)
{
    if (s_mounted)
        return true;
    if (!bsp_sdcard_is_inserted())
        return false;
    if (bsp_sdcard_init_default() != ESP_OK) {
        ESP_LOGE(TAG, "Card is in but would not mount");
        return false;
    }
    s_mounted = true;
    return true;
}

/// Highest REC%05u.WAV already on the card, plus one. FAT here is 8.3 only,
/// so the names are counted, not dated.
static unsigned next_index(void)
{
    DIR *d = opendir(REC_DIR);
    if (!d)
        return 1;

    unsigned best = 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncasecmp(e->d_name, "REC", 3) != 0)
            continue;
        const char *p = e->d_name + 3;
        unsigned n = 0;
        while (*p >= '0' && *p <= '9')
            n = n * 10 + (unsigned)(*p++ - '0');
        if (strcasecmp(p, ".WAV") == 0 && n > best)
            best = n;
    }
    closedir(d);
    return best + 1;
}

static bool wav_open(void)
{
    mkdir(REC_DIR, 0777);        /* usually already there; nothing to do */

    snprintf(s_path, sizeof s_path, REC_DIR "/REC%05u.WAV", next_index());
    s_file = fopen(s_path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "Could not open %s", s_path);
        return false;
    }

    const wav_hdr_t h = wav_header(0);
    if (fwrite(&h, sizeof h, 1, s_file) != 1) {
        ESP_LOGE(TAG, "Could not write the header of %s", s_path);
        fclose(s_file);
        s_file = NULL;
        return false;
    }
    return true;
}

static void wav_close(uint32_t data_bytes)
{
    const wav_hdr_t h = wav_header(data_bytes);
    if (fseek(s_file, 0, SEEK_SET) != 0 || fwrite(&h, sizeof h, 1, s_file) != 1)
        ESP_LOGE(TAG, "Could not patch the header of %s", s_path);
    fclose(s_file);
    s_file = NULL;
}

// --- Tasks ---

static void notify_state(bool recording)
{
    if (s_state_cb)
        s_state_cb(recording);
}

/// Nothing but the mic. Any work done here is work the DMA is not doing.
static void reader_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!atomic_load(&s_pulling)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        size_t got = 0;
        if (bsp_i2s_read(s_chunk, sizeof s_chunk, &got, 1000) != ESP_OK || got == 0)
            continue;
        if (xStreamBufferSend(s_ring, s_chunk, got, 0) != got)
            s_dropped++;    /* the card is four seconds behind: give up on it */
    }
}

static void record(void)
{
    if (!sd_ready()) {
        ESP_LOGW(TAG, "No card in the slot, nothing to record onto");
        atomic_store(&s_want, false);
        return;
    }

    /* quiet first: the file should not open onto the sound of the pond */
    sound_set_muted(true);
    if (!wav_open()) {
        sound_set_muted(false);
        atomic_store(&s_want, false);
        return;
    }

    xSemaphoreTake(s_done, 0);           /* drop any stale completion */
    xStreamBufferReset(s_ring);
    s_dropped = 0;

    atomic_store(&s_pulling, true);
    xTaskNotifyGive(s_reader_task);
    notify_state(true);

    size_t total = 0;
    bool card_gave_up = false;
    while (atomic_load(&s_want) && total < MAX_BYTES) {
        size_t n = xStreamBufferReceive(s_ring, s_buf, sizeof s_buf, pdMS_TO_TICKS(100));
        if (n == 0)
            continue;
        if (fwrite(s_buf, 1, n, s_file) != n) {
            ESP_LOGE(TAG, "Write failed, card full or gone");
            card_gave_up = true;
            break;
        }
        total += n;
    }

    atomic_store(&s_pulling, false);

    /* A click clears s_want itself, and one that lands while we are closing
     * up should start the next recording rather than be swallowed here. So
     * only the reasons that stop on their own clear the flag. */
    if (card_gave_up || total >= MAX_BYTES)
        atomic_store(&s_want, false);

    if (!card_gave_up) {
        /* whatever the reader had already taken still belongs in the file */
        size_t n;
        while ((n = xStreamBufferReceive(s_ring, s_buf, sizeof s_buf,
                                         pdMS_TO_TICKS(DRAIN_MS))) > 0) {
            if (fwrite(s_buf, 1, n, s_file) != n)
                break;
            total += n;
        }
    }

    wav_close((uint32_t)total);
    if (card_gave_up) {
        /* it may simply have been pulled out: drop the mount so the next
         * click goes and looks for a card again */
        bsp_sdcard_deinit_default();
        s_mounted = false;
    }
    sound_set_muted(false);
    notify_state(false);
    xSemaphoreGive(s_done);

    unsigned ms = (unsigned)(total * 1000 / REC_BYTES_PER_SEC);
    ESP_LOGI(TAG, "Saved %s: %u.%us, %u bytes%s", s_path, ms / 1000,
             (ms % 1000) / 100, (unsigned)total,
             s_dropped ? " (samples dropped)" : "");
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load(&s_want))
            record();        /* returns when the file has been closed */
    }
}

// --- Public API ---

void recorder_init(void)
{
    if (s_ring)
        return;

    s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_done = xSemaphoreCreateBinary();
    if (!s_ring || !s_done) {
        ESP_LOGE(TAG, "Out of room for the recording ring");
        return;
    }

    /* the reader outranks the writer: a late write is absorbed by the ring,
     * a late read is a hole in the recording */
    if (xTaskCreate(reader_task, "rec_read", 4096, NULL, 6, &s_reader_task) != pdPASS ||
        xTaskCreate(writer_task, "rec_write", 6144, NULL, 4, &s_writer_task) != pdPASS)
        ESP_LOGE(TAG, "Failed to start the recorder tasks");
}

void recorder_toggle(void)
{
    if (!s_writer_task)
        return;

    if (atomic_load(&s_want)) {
        atomic_store(&s_want, false);   /* the writer drains and closes up */
        return;
    }
    atomic_store(&s_want, true);
    xTaskNotifyGive(s_writer_task);
}

bool recorder_is_active(void)
{
    return atomic_load(&s_want);
}

void recorder_set_state_cb(void (*cb)(bool recording))
{
    s_state_cb = cb;
}

void recorder_flush(uint32_t timeout_ms)
{
    if (!atomic_load(&s_want))
        return;
    atomic_store(&s_want, false);
    xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms));
}
