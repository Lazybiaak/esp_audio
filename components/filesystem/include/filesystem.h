#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "esp_err.h"

esp_err_t filesystem_init(void);
esp_err_t filesystem_test(void);

#endif