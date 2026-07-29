#include "downloader.h"

#include <stdio.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "DOWNLOADER";

#define DOWNLOAD_BUFFER_SIZE 1024

esp_err_t downloader_download_file(const char *url,
                                   const char *destination)
{
    ESP_LOGI(TAG, "URL: %s", url);
    ESP_LOGI(TAG, "Saving to: %s", destination);

    FILE *fp = fopen(destination, "wb");
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "Cannot create %s", destination);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Received URL: %s", url);
    esp_http_client_config_t config = {
    .url = url,
    .timeout_ms = 10000,
    .crt_bundle_attach = esp_crt_bundle_attach,
};

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL)
    {
        fclose(fp);
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(fp);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);

    ESP_LOGI(TAG, "Content Length = %d", content_length);

    char buffer[DOWNLOAD_BUFFER_SIZE];

    int total = 0;

    while (1)
    {
        int read = esp_http_client_read(
                        client,
                        buffer,
                        sizeof(buffer));

        if (read < 0)
        {
            ESP_LOGE(TAG, "Read error");
            break;
        }

        if (read == 0)
        {
            break;
        }

        fwrite(buffer, 1, read, fp);

        total += read;

        ESP_LOGI(TAG,
                 "Downloaded %d bytes",
                 total);
    }

    fclose(fp);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Download Complete");

    return ESP_OK;
}