#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include "esp_err.h"

esp_err_t downloader_download_file(
    const char *url,
    const char *path
);

#endif