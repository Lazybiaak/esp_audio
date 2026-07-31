#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "driver/touch_sensor.h"

#include "wifi_manager.h"
#include "downloader.h"
#include "aud_player.h"


#define TAG "MAIN"


/* ---- Wi-Fi + source: set these to your own values -------------------- */
#define WIFI_SSID       "Ball"
#define WIFI_PASSWORD   "apple123"
#define WAV_URL         "https://raw.githubusercontent.com/lazybiaak/esp_audio/main/hello.wav"

/* ---- Touch buttons --------------------------------------------------- */
#define PAD_DOWNLOAD    TOUCH_PAD_NUM0    // GPIO4  (D4)  -> Button 1: download & store
#define PAD_PLAY        TOUCH_PAD_NUM5    // GPIO12       -> Button 2: playback
// NOTE: GPIO12 (T5) is a boot strapping pin (MTDI). Make sure nothing pulls it
// HIGH at reset (don't touch the pad while the board is booting) or the flash
// voltage select can prevent boot.

/* Raw flash partition that holds the downloaded WAV (see partitions.csv). */
#define AUDIO_PART_LABEL "audio"

/* Touch: a pad is "touched" when its reading drops below 4/5 of baseline. */
#define TRIGGER_NUM     4
#define TRIGGER_DEN     5
#define POLL_MS         50


/* ---- State machine --------------------------------------------------- */
typedef enum { STATE_IDLE, STATE_DOWNLOADING, STATE_PLAYING } app_state_t;
typedef enum { ACT_DOWNLOAD, ACT_PLAY }                       action_t;

static app_state_t       s_state     = STATE_IDLE;
static SemaphoreHandle_t s_state_mtx = NULL;   // guards s_state
static QueueHandle_t     s_action_q  = NULL;   // touch task -> worker task


static const touch_pad_t s_pads[] = { PAD_DOWNLOAD, PAD_PLAY };
#define NUM_PADS   (sizeof(s_pads) / sizeof(s_pads[0]))
static uint16_t s_threshold[NUM_PADS];


/*
 * Atomically move IDLE -> target. Returns true only if we claimed the state,
 * which is what guarantees a download and a playback can never overlap.
 */
static bool state_claim(app_state_t target)
{
    bool ok = false;

    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    if (s_state == STATE_IDLE)
    {
        s_state = target;
        ok = true;
    }
    xSemaphoreGive(s_state_mtx);

    return ok;
}

static void state_release(void)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_state = STATE_IDLE;
    xSemaphoreGive(s_state_mtx);
}


static const esp_partition_t *audio_partition(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, AUDIO_PART_LABEL);

    if (p == NULL)
    {
        ESP_LOGE(TAG, "Partition '%s' not found", AUDIO_PART_LABEL);
    }
    return p;
}


/* ---- Capacitive touch ------------------------------------------------ */
static void touch_init(void)
{
    ESP_ERROR_CHECK(touch_pad_init());
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);

    for (size_t i = 0; i < NUM_PADS; i++)
    {
        ESP_ERROR_CHECK(touch_pad_config(s_pads[i], 0));
    }

    vTaskDelay(pdMS_TO_TICKS(200));   // let the hardware settle

    for (size_t i = 0; i < NUM_PADS; i++)
    {
        uint16_t val = 0;
        touch_pad_read(s_pads[i], &val);
        s_threshold[i] = (uint16_t)((uint32_t)val * TRIGGER_NUM / TRIGGER_DEN);

        ESP_LOGI(TAG, "Touch pad %d: baseline=%u, threshold=%u",
                 s_pads[i], val, s_threshold[i]);
    }
}

static bool pad_touched(size_t i)
{
    uint16_t val = 0;
    return touch_pad_read(s_pads[i], &val) == ESP_OK && val < s_threshold[i];
}


/*
 * Low-priority monitor task. Only detects fresh presses and dispatches an
 * action to the worker; it never blocks on the heavy work itself, so touch
 * sensing stays responsive.
 */
static void touch_task(void *arg)
{
    bool armed[NUM_PADS];
    for (size_t i = 0; i < NUM_PADS; i++)
    {
        armed[i] = true;
    }

    while (1)
    {
        for (size_t i = 0; i < NUM_PADS; i++)
        {
            if (pad_touched(i))
            {
                if (armed[i])
                {
                    armed[i] = false;

                    action_t    act  = (s_pads[i] == PAD_DOWNLOAD)
                                           ? ACT_DOWNLOAD : ACT_PLAY;
                    app_state_t want = (act == ACT_DOWNLOAD)
                                           ? STATE_DOWNLOADING : STATE_PLAYING;

                    if (state_claim(want))
                    {
                        ESP_LOGI(TAG, "Pad %d -> %s", s_pads[i],
                                 act == ACT_DOWNLOAD ? "DOWNLOAD" : "PLAY");
                        xQueueSend(s_action_q, &act, 0);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Busy, ignoring pad %d", s_pads[i]);
                    }
                }
            }
            else
            {
                armed[i] = true;   // released: ready for the next touch
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}


/* ---- Worker: performs one heavy action at a time --------------------- */
static void do_download(void)
{
    static bool s_wifi_started = false;   // wifi_manager_init() must run only once

    const esp_partition_t *part = audio_partition();
    if (part == NULL)
    {
        return;
    }

    if (!s_wifi_started)
    {
        ESP_LOGI(TAG, "Bringing up Wi-Fi (ssid=%s)...", WIFI_SSID);

        if (wifi_manager_init(WIFI_SSID, WIFI_PASSWORD) != ESP_OK)
        {
            ESP_LOGE(TAG, "Wi-Fi init failed");
            return;
        }
        s_wifi_started = true;
    }

    if (!wifi_manager_is_connected())
    {
        // Wait up to ~15 s for an IP (also covers a dropped/reconnecting link).
        for (int i = 0; i < 150 && !wifi_manager_is_connected(); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (!wifi_manager_is_connected())
    {
        ESP_LOGE(TAG, "No Wi-Fi connection, aborting download");
        return;
    }

    esp_err_t err = downloader_download_to_partition(WAV_URL, part);
    ESP_LOGI(TAG, "Download result: %s", esp_err_to_name(err));
}

static void do_play(void)
{
    const esp_partition_t *part = audio_partition();
    if (part == NULL)
    {
        return;
    }

    esp_err_t err = aud_player_play_partition(part);
    ESP_LOGI(TAG, "Playback result: %s", esp_err_to_name(err));
}

static void worker_task(void *arg)
{
    action_t act;

    while (1)
    {
        if (xQueueReceive(s_action_q, &act, portMAX_DELAY) == pdTRUE)
        {
            if (act == ACT_DOWNLOAD)
            {
                do_download();
            }
            else
            {
                do_play();
            }

            state_release();   // back to IDLE; buttons are live again
        }
    }
}


void app_main(void)
{
    printf("\n");
    printf("=====================================\n");
    printf(" ESP32 Touch Audio (download / play)\n");
    printf("=====================================\n");


    s_state_mtx = xSemaphoreCreateMutex();
    s_action_q  = xQueueCreate(4, sizeof(action_t));

    if (s_state_mtx == NULL || s_action_q == NULL)
    {
        ESP_LOGE(TAG, "Failed to create state primitives");
        return;
    }

    const esp_partition_t *part = audio_partition();
    if (part != NULL)
    {
        ESP_LOGI(TAG, "audio partition: %u bytes @ 0x%06x",
                 (unsigned)part->size, (unsigned)part->address);
    }

    touch_init();

    xTaskCreate(worker_task, "worker", 8192, NULL, 5, NULL);   // does the work
    xTaskCreate(touch_task,  "touch",  4096, NULL, 3, NULL);   // low-prio monitor

    ESP_LOGI(TAG, "Ready. Touch D4/GPIO4 = download, GPIO12 = play.");
}
