#include "downloader.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "DOWNLOADER";

#define DOWNLOAD_BUFFER_SIZE 1024
#define FLASH_SECTOR_BYTES   4096

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


esp_err_t downloader_download_to_partition(const char *url,
                                           const esp_partition_t *part)
{
    if (url == NULL || part == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Streaming %s -> partition '%s' (%u bytes)",
             url, part->label, (unsigned)part->size);

    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status         = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP status = %d, Content-Length = %d",
             status, content_length);

    if (content_length > 0 && (size_t)content_length > part->size)
    {
        ESP_LOGE(TAG, "File (%d) larger than partition (%u)",
                 content_length, (unsigned)part->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    uint8_t        tmp[DOWNLOAD_BUFFER_SIZE];
    static uint8_t sector[FLASH_SECTOR_BYTES];   // static: keep it off the stack
    size_t         sec_fill = 0;                 // bytes buffered in 'sector'
    size_t         part_off = 0;                 // write cursor into the partition
    int            total    = 0;

    while (1)
    {
        int r = esp_http_client_read(client, (char *)tmp, sizeof(tmp));

        if (r < 0)
        {
            ESP_LOGE(TAG, "Read error");
            err = ESP_FAIL;
            break;
        }

        if (r == 0)
        {
            break;                               // end of stream
        }

        // Pack incoming bytes into full 4 KB sectors, flushing as we go.
        size_t consumed = 0;

        while (consumed < (size_t)r)
        {
            size_t space = FLASH_SECTOR_BYTES - sec_fill;
            size_t n     = space < (size_t)r - consumed
                               ? space
                               : (size_t)r - consumed;

            memcpy(sector + sec_fill, tmp + consumed, n);
            sec_fill += n;
            consumed += n;

            if (sec_fill == FLASH_SECTOR_BYTES)
            {
                if (part_off + FLASH_SECTOR_BYTES > part->size)
                {
                    ESP_LOGE(TAG, "Partition overflow at %u",
                             (unsigned)part_off);
                    err = ESP_ERR_NO_MEM;
                    goto done;
                }

                // Erase the target sector, then write it.
                err = esp_partition_erase_range(part, part_off,
                                                FLASH_SECTOR_BYTES);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
                    goto done;
                }

                err = esp_partition_write(part, part_off, sector,
                                          FLASH_SECTOR_BYTES);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(err));
                    goto done;
                }

                part_off += FLASH_SECTOR_BYTES;
                sec_fill  = 0;
            }
        }

        total += r;
    }

    // Flush the final partial sector, zero-padded.
    if (err == ESP_OK && sec_fill > 0)
    {
        if (part_off + FLASH_SECTOR_BYTES > part->size)
        {
            ESP_LOGE(TAG, "Partition overflow on final sector");
            err = ESP_ERR_NO_MEM;
        }
        else
        {
            memset(sector + sec_fill, 0, FLASH_SECTOR_BYTES - sec_fill);

            err = esp_partition_erase_range(part, part_off,
                                            FLASH_SECTOR_BYTES);
            if (err == ESP_OK)
            {
                err = esp_partition_write(part, part_off, sector,
                                          FLASH_SECTOR_BYTES);
            }
        }
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Download complete: %d bytes written to '%s'",
                 total, part->label);
    }
    else
    {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(err));
    }

    return err;
}