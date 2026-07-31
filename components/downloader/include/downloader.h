#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include "esp_err.h"
#include "esp_partition.h"

/* Download a URL into a file on a mounted filesystem (existing behaviour). */
esp_err_t downloader_download_file(
    const char *url,
    const char *path
);

/*
 * Stream a URL directly into a raw flash partition using the esp_partition
 * API. The data is written sequentially, erasing each 4 KB sector just before
 * it is written, and never buffering the whole file in RAM. The final partial
 * sector is zero-padded. Fails with ESP_ERR_NO_MEM if the file is larger than
 * the partition.
 */
esp_err_t downloader_download_to_partition(
    const char *url,
    const esp_partition_t *partition
);

#endif
