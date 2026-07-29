#include <stdio.h>
#include <string.h>

#include "filesystem.h"

#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "FILESYSTEM";

esp_err_t filesystem_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount LittleFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;

    ret = esp_littlefs_info(conf.partition_label, &total, &used);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "LittleFS mounted");
        ESP_LOGI(TAG, "Total : %zu bytes", total);
        ESP_LOGI(TAG, "Used  : %zu bytes", used);
    }

    return ESP_OK;
}

esp_err_t filesystem_test(void)
{
    const char *filename = "/storage/test.txt";

    ESP_LOGI(TAG, "Creating %s", filename);

    FILE *fp = fopen(filename, "w");

    if (fp == NULL)
    {
        ESP_LOGE(TAG, "Cannot create file");
        return ESP_FAIL;
    }

    fprintf(fp, "Hello from ESP32!\n");

    fclose(fp);

    fp = fopen(filename, "r");

    if (fp == NULL)
    {
        ESP_LOGE(TAG, "Cannot open file");
        return ESP_FAIL;
    }

    char buffer[128];

    if (fgets(buffer, sizeof(buffer), fp))
    {
        ESP_LOGI(TAG, "Contents: %s", buffer);
    }

    fclose(fp);

    remove(filename);

    ESP_LOGI(TAG, "File deleted");

    return ESP_OK;
}