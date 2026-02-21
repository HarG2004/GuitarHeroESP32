// main.c — Robust SDMMC(1-bit) + FATFS + WAV(16-bit PCM) -> I2S (PCM5102)
//
// Key stability changes:
//   1) SDMMC clock increased (400 kHz -> 10 MHz) so reads can keep up with audio.
//   2) Recovery path now closes FILE* BEFORE unmounting SD, then remounts + reopens + seeks.
//
// NOTE: Hardware still matters a lot for SD stability.
//       External ~10k pull-ups on CMD/D0..D3 + solid 3.3V decoupling are strongly recommended.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"

// Tag for main file
#define TAG "MAIN"

// Mount on /sdcard with file Song1.wav for now
#define MOUNT_POINT "/sdcard"
#define WAV_PATH    MOUNT_POINT "/Song1.wav"   // ensure exact case + extension

// SDMMC pins 
#define SDMMC_CLK   GPIO_NUM_14
#define SDMMC_CMD   GPIO_NUM_15
#define SDMMC_D0    GPIO_NUM_2

// I2S pins
#define I2S_LRCK_PIN GPIO_NUM_25
#define I2S_BCK_PIN  GPIO_NUM_26
#define I2S_DATA_PIN GPIO_NUM_27

// Recovery and Freq settings
#define SDMMC_FREQ_KHZ          20000  // was 400; 10 MHz is a sane starting point for streaming
#define SD_MOUNT_RETRIES        5
#define STREAM_RECOVER_RETRIES  5

// Little Edeian helper methods
static uint16_t le16(const uint8_t *p) { 
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8); 
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// struct for wave file info header
typedef struct {
    uint16_t fmt;              // 1 = PCM
    uint16_t ch;               // 1 or 2
    uint32_t rate;             // sample rate
    uint16_t bits;             // bits per sample (expect 16)
    uint32_t data_offset;
    uint32_t data_size;
} wav_info_t;

// For recovery in case of sd card communication failure
static sdmmc_card_t *g_card = NULL;
static i2s_chan_handle_t g_tx = NULL;

// Method to pull up sdmmc pins
static void sd_enable_pullups(void){
    gpio_set_pull_mode(SDMMC_CMD, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SDMMC_D0,  GPIO_PULLUP_ONLY);
}

// Method to mount sd card
static esp_err_t mount_sd(void){
    sd_enable_pullups(); // enable pull ups

    // Configure sdmmc settings
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SDMMC_CLK;
    slot.cmd = SDMMC_CMD;
    slot.d0  = SDMMC_D0;

    // Force internal pullups inside the SDMMC driver too
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // mount settings
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 4096,
    };

    // Mount sd card
    for (int i = 1; i <= SD_MOUNT_RETRIES; i++) {
        ESP_LOGI(TAG, "SD mount attempt %d...", i);
        sdmmc_card_t *card = NULL;
        esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &cfg, &card);

        if (err == ESP_OK) {
            g_card = card;
            ESP_LOGI(TAG, "SD mounted");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Mount failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return ESP_FAIL;
}

// Method to unmount sdcard
static void unmount_sd(void)
{
    if (g_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, g_card);
        g_card = NULL;
        ESP_LOGI(TAG, "SD unmounted");
    }
}

// Method to parse wave file
static esp_err_t wav_parse(FILE *f, wav_info_t *w){
    uint8_t riff[12];

    if (fread(riff, 1, 12, f) != 12) return ESP_FAIL;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return ESP_ERR_INVALID_ARG;

    memset(w, 0, sizeof(*w));
    bool got_fmt = false, got_data = false;

    while (!got_data) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) return ESP_FAIL;

        uint32_t chunk_size = le32(hdr + 4);

        if (memcmp(hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16) return ESP_ERR_INVALID_SIZE;
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) return ESP_FAIL;

            w->fmt  = le16(fmt + 0);
            w->ch   = le16(fmt + 2);
            w->rate = le32(fmt + 4);
            w->bits = le16(fmt + 14);

            if (chunk_size > 16) fseek(f, (long)(chunk_size - 16), SEEK_CUR);
            got_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            w->data_offset = (uint32_t)ftell(f);
            w->data_size   = chunk_size;
            got_data = true;
        } else {
            fseek(f, (long)chunk_size, SEEK_CUR);
        }

        if (chunk_size & 1) fseek(f, 1, SEEK_CUR);
    }

    return got_fmt ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// Method to start I2S protocal to amplifier
static esp_err_t i2s_start(uint32_t sample_rate) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &g_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_LRCK_PIN,
            .dout = I2S_DATA_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv=false, .bclk_inv=false, .ws_inv=false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_tx));
    return ESP_OK;
}

// Stop I2S protocal
static void i2s_stop(void){
    if (g_tx) {
        i2s_channel_disable(g_tx);
        i2s_del_channel(g_tx);
        g_tx = NULL;
    }
}

// Method to re open and read file
static esp_err_t reopen_and_seek(FILE **pf, const char *path, uint32_t data_offset, uint32_t resume_data_bytes){
    if (*pf) {
        fclose(*pf);
        *pf = NULL;
    }

    *pf = fopen(path, "rb");
    if (!*pf) return ESP_FAIL;

    // Seek to start of data chunk + where we left off within data
    if (fseek(*pf, (long)data_offset, SEEK_SET) != 0) return ESP_FAIL;
    if (resume_data_bytes > 0) {
        if (fseek(*pf, (long)resume_data_bytes, SEEK_CUR) != 0) return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t play_wav_with_recovery(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Open failed: %s", path);
        return ESP_FAIL;
    }

    wav_info_t w;
    esp_err_t err = wav_parse(f, &w);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WAV parse failed: %s", esp_err_to_name(err));
        fclose(f);
        return err;
    }

    ESP_LOGI(TAG, "WAV: fmt=%u ch=%u rate=%lu bits=%u data=%lu",
             w.fmt, w.ch, (unsigned long)w.rate, w.bits, (unsigned long)w.data_size);

    if (w.fmt != 1 || w.bits != 16 || (w.ch != 1 && w.ch != 2)) {
        ESP_LOGE(TAG, "Need PCM (fmt=1) 16-bit mono/stereo.");
        fclose(f);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(i2s_start(w.rate));

    // Seek to data
    fseek(f, (long)w.data_offset, SEEK_SET);

    // DMA-safe buffers (INTERNAL|DMA), not PSRAM
    const size_t IN_BYTES = 4096;
    uint8_t *inbuf = heap_caps_malloc(IN_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int32_t *out32  = heap_caps_malloc(IN_BYTES * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!inbuf || !out32) {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        if (inbuf) heap_caps_free(inbuf);
        if (out32) heap_caps_free(out32);
        fclose(f);
        i2s_stop();
        return ESP_ERR_NO_MEM;
    }

    uint32_t remaining = w.data_size;
    uint32_t played_data_bytes = 0; // bytes consumed from WAV data chunk

    int recover_budget = STREAM_RECOVER_RETRIES;

    while (remaining > 0) {
        size_t want = remaining > IN_BYTES ? IN_BYTES : remaining;
        size_t got  = fread(inbuf, 1, want, f);

        if (got == 0) {
            if (ferror(f)) {
                ESP_LOGW(TAG, "Read error mid-stream. Attempting recovery...");
                clearerr(f);

                if (recover_budget-- <= 0) {
                    ESP_LOGE(TAG, "Recovery budget exhausted.");
                    break;
                }

                // IMPORTANT: close the FILE* before unmounting the filesystem
                fclose(f);
                f = NULL;

                unmount_sd();
                vTaskDelay(pdMS_TO_TICKS(500)); // let the card/bus settle a bit

                if (mount_sd() != ESP_OK) {
                    ESP_LOGE(TAG, "Remount failed during recovery.");
                    break;
                }

                if (reopen_and_seek(&f, path, w.data_offset, played_data_bytes) != ESP_OK) {
                    ESP_LOGE(TAG, "Reopen/seek failed during recovery.");
                    break;
                }

                continue;
            }
            // EOF
            break;
        }

        remaining -= got;
        played_data_bytes += got;

        // Convert to 32-bit stereo frames
        size_t frames = 0;
        if (w.ch == 1) {
            size_t samples = got / 2;
            int16_t *s = (int16_t *)inbuf;
            for (size_t i = 0; i < samples; i++) {
                int32_t v = ((int32_t)s[i]) << 16;
                out32[2*i + 0] = v;
                out32[2*i + 1] = v;
            }
            frames = samples;
        } else {
            size_t samples = got / 2;          // int16 count
            size_t stereo_frames = samples / 2;
            int16_t *s = (int16_t *)inbuf;
            for (size_t i = 0; i < stereo_frames; i++) {
                out32[2*i + 0] = ((int32_t)s[2*i + 0]) << 16;
                out32[2*i + 1] = ((int32_t)s[2*i + 1]) << 16;
            }
            frames = stereo_frames;
        }

        size_t bytes_to_write = frames * 2 * sizeof(int32_t);
        size_t bytes_written  = 0;
        esp_err_t we = i2s_channel_write(g_tx, out32, bytes_to_write, &bytes_written, portMAX_DELAY);
        if (we != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(we));
            break;
        }
    }

    // Cleanup
    heap_caps_free(inbuf);
    heap_caps_free(out32);

    if (f) fclose(f);

    vTaskDelay(pdMS_TO_TICKS(150));
    i2s_stop();

    ESP_LOGI(TAG, "Playback finished (or stopped).");
    return ESP_OK;
}

void app_main(void){
    
    ESP_LOGI(TAG, "Mounting SD...");

    if (mount_sd() != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed.");
        return;
    }

    ESP_LOGI(TAG, "Playing %s", WAV_PATH);
    (void)play_wav_with_recovery(WAV_PATH);

    unmount_sd();
}
