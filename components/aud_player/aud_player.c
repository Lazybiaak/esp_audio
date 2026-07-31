#include "aud_player.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_partition.h"

#include "driver/dac_continuous.h"


#define TAG "AUD_PLAYER"

// Input PCM samples pulled from the source per iteration.
#define IN_SAMPLES   512

// Output the DAC at OVERSAMPLE x the source rate, linearly interpolating
// between samples. This smooths the 8-bit "stair-steps" and moves the
// imaging noise up above the audible band, giving noticeably cleaner sound.
#define OVERSAMPLE   4


static dac_continuous_handle_t s_dac_handle = NULL;


/*
 * A source of PCM bytes. Returns the number of bytes actually read into buf
 * (0 at end of stream). Lets the same engine play from a file or a partition.
 */
typedef size_t (*pcm_reader_t)(void *ctx, void *buf, size_t len);


// ---- File source ----------------------------------------------------------
static size_t file_reader(void *ctx, void *buf, size_t len)
{
    return fread(buf, 1, len, (FILE *)ctx);
}

// ---- Partition source -----------------------------------------------------
typedef struct
{
    const esp_partition_t *part;
    size_t                 offset;
    size_t                 end;
} part_ctx_t;

static size_t part_reader(void *vctx, void *buf, size_t len)
{
    part_ctx_t *c = (part_ctx_t *)vctx;

    if (c->offset >= c->end)
    {
        return 0;
    }

    size_t n = c->end - c->offset;
    if (n > len)
    {
        n = len;
    }

    if (esp_partition_read(c->part, c->offset, buf, n) != ESP_OK)
    {
        return 0;
    }

    c->offset += n;
    return n;
}


// Read and discard n bytes from the source (used to skip WAV chunks).
static void reader_skip(pcm_reader_t rd, void *ctx, size_t n)
{
    uint8_t scratch[64];

    while (n > 0)
    {
        size_t chunk = n < sizeof(scratch) ? n : sizeof(scratch);
        size_t got   = rd(ctx, scratch, chunk);

        if (got == 0)
        {
            break;
        }
        n -= got;
    }
}


// The handful of WAV fields we actually need.
typedef struct
{
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_size;
} wav_info_t;


// Parse RIFF chunks sequentially; leaves the source positioned at the first
// PCM sample of the "data" chunk.
static esp_err_t wav_parse(pcm_reader_t rd, void *ctx, wav_info_t *out)
{
    uint8_t riff[12];

    if (rd(ctx, riff, 12) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0)
    {
        ESP_LOGE(TAG, "Not a RIFF/WAVE stream");
        return ESP_FAIL;
    }

    bool have_fmt  = false;
    bool have_data = false;
    uint8_t chunk[8];

    while (rd(ctx, chunk, 8) == 8)
    {
        uint32_t sz = chunk[4] |
                     (chunk[5] << 8) |
                     (chunk[6] << 16) |
                     ((uint32_t)chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            uint8_t fmt[16];
            uint32_t n = sz < sizeof(fmt) ? sz : sizeof(fmt);

            if (rd(ctx, fmt, n) != n)
            {
                return ESP_FAIL;
            }

            out->num_channels    = fmt[2] | (fmt[3] << 8);
            out->sample_rate     = fmt[4] | (fmt[5] << 8) |
                                  (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            out->bits_per_sample = fmt[14] | (fmt[15] << 8);

            if (sz > n)
            {
                reader_skip(rd, ctx, sz - n);
            }
            have_fmt = true;
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            out->data_size = sz;
            have_data = true;
            break;                      // positioned at the first sample
        }
        else
        {
            reader_skip(rd, ctx, sz + (sz & 1));   // skip unknown chunk
        }
    }

    if (!have_fmt || !have_data)
    {
        ESP_LOGE(TAG, "Missing fmt/data chunk");
        return ESP_FAIL;
    }

    return ESP_OK;
}


// Shared playback engine: parse header, configure the DAC, then stream and
// play the PCM data via the given reader.
static esp_err_t play_stream(pcm_reader_t rd, void *ctx)
{
    wav_info_t wav = {0};
    esp_err_t  err = wav_parse(rd, ctx, &wav);

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "WAV: %u Hz, %u-bit, %u ch, %u data bytes",
             (unsigned)wav.sample_rate, wav.bits_per_sample,
             wav.num_channels, (unsigned)wav.data_size);

    if (wav.bits_per_sample != 16 && wav.bits_per_sample != 8)
    {
        ESP_LOGE(TAG, "Unsupported bit depth %u", wav.bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }


    // DAC in continuous (DMA) mode. GPIO25 = DAC channel 0. APLL clock so low
    // rates (16 kHz) are reachable; OVERSAMPLE raises the actual output rate.
    dac_continuous_config_t cont_cfg =
    {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num  = 8,
        .buf_size  = 2048,
        .freq_hz   = wav.sample_rate * OVERSAMPLE,
        .offset    = 0,
        .clk_src   = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };

    err = dac_continuous_new_channels(&cont_cfg, &s_dac_handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "dac_continuous_new_channels failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(dac_continuous_enable(s_dac_handle));


    uint8_t  in[IN_SAMPLES * 2];             // room for 16-bit samples
    uint8_t  out[IN_SAMPLES * OVERSAMPLE];   // interpolated 8-bit DAC output
    uint32_t remaining = wav.data_size;
    int      prev = -1;                      // last DAC level (0..255); -1 = none yet

    const size_t max_bytes = (wav.bits_per_sample == 16)
                                 ? IN_SAMPLES * 2
                                 : IN_SAMPLES;

    while (remaining > 0)
    {
        size_t want = remaining < max_bytes ? remaining : max_bytes;
        size_t got  = rd(ctx, in, want);

        if (got == 0)
        {
            break;                      // short/truncated stream
        }
        remaining -= got;


        size_t n_samples = (wav.bits_per_sample == 16) ? got / 2 : got;
        size_t out_len   = 0;

        for (size_t i = 0; i < n_samples; i++)
        {
            int level;

            if (wav.bits_per_sample == 16)
            {
                // 16-bit signed little-endian -> 8-bit unsigned, clamped.
                int16_t s = (int16_t)(in[2 * i] | (in[2 * i + 1] << 8));
                int v = ((int)s + 32768) >> 8;
                level = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
            else
            {
                level = in[i];          // 8-bit WAV data is already 0..255
            }

            if (prev < 0)
            {
                prev = level;           // prime on the very first sample
            }

            // Linearly interpolate prev -> level across OVERSAMPLE points.
            for (int k = 0; k < OVERSAMPLE; k++)
            {
                out[out_len++] = (uint8_t)(prev + (level - prev) * k / OVERSAMPLE);
            }

            prev = level;
        }


        // Blocking DMA write: the task sleeps while the buffers drain, so the
        // idle task runs and the task watchdog stays fed.
        size_t total = 0;

        while (total < out_len)
        {
            size_t written = 0;

            if (dac_continuous_write(s_dac_handle,
                                     out + total,
                                     out_len - total,
                                     &written,
                                     -1) != ESP_OK)
            {
                break;
            }
            total += written;
        }
    }


    // Let the final DMA buffers play out before tearing the channel down.
    vTaskDelay(pdMS_TO_TICKS(50));

    dac_continuous_disable(s_dac_handle);
    dac_continuous_del_channels(s_dac_handle);
    s_dac_handle = NULL;

    ESP_LOGI(TAG, "Playback completed");

    return ESP_OK;
}


esp_err_t aud_player_play(const char *filename)
{
    ESP_LOGI(TAG, "Playing %s", filename);

    FILE *file = fopen(filename, "rb");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open WAV");
        return ESP_FAIL;
    }

    esp_err_t err = play_stream(file_reader, file);

    fclose(file);
    return err;
}


esp_err_t aud_player_play_partition(const esp_partition_t *part)
{
    if (part == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Playing from partition '%s'", part->label);

    part_ctx_t ctx =
    {
        .part   = part,
        .offset = 0,
        .end    = part->size,
    };

    return play_stream(part_reader, &ctx);
}
