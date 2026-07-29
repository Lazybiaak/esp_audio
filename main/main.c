#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "filesystem.h"
#include "wifi_manager.h"
#include "downloader.h"
#include "aud_player.h"


#define WIFI_SSID      "Ball"
#define WIFI_PASSWORD  "apple123"



static void download_task(void *pvParameters)
{
    ESP_LOGI("MAIN", "Starting download...");


    esp_err_t err = downloader_download_file(
        "https://raw.githubusercontent.com/Lazybiaak/portfolio/main/img/hello.wav",
        "/storage/hello.wav"
    );


    if (err == ESP_OK)
    {
        ESP_LOGI("MAIN", "Download successful!");

        aud_player_play("/storage/hello.wav");
    }
    else
    {
        ESP_LOGE(
            "MAIN",
            "Download failed: %s",
            esp_err_to_name(err)
        );
    }


    vTaskDelete(NULL);
}




void app_main(void)
{
    printf("\n");
    printf("=====================================\n");
    printf(" ESP32 WAV Downloader\n");
    printf("=====================================\n");


    /*
     * Mount LittleFS
     */
    ESP_ERROR_CHECK(
        filesystem_init()
    );


    filesystem_test();



    /*
     * Connect WiFi
     */
    ESP_ERROR_CHECK(
        wifi_manager_init(
            WIFI_SSID,
            WIFI_PASSWORD
        )
    );


    printf("Waiting for WiFi");


    while(!wifi_manager_is_connected())
    {
        printf(".");
        fflush(stdout);

        vTaskDelay(
            pdMS_TO_TICKS(500)
        );
    }


    printf("\nWiFi Connected!\n");



    /*
     * Download WAV
     */
    xTaskCreate(
        download_task,
        "download_task",
        16384,
        NULL,
        5,
        NULL
    );



    while(1)
    {
        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}