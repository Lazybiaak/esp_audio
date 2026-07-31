#ifndef AUD_PLAYER_H
#define AUD_PLAYER_H

#include "esp_err.h"
#include "esp_partition.h"

/* Play a WAV file from a mounted filesystem path. */
esp_err_t aud_player_play(const char *filename);

/* Play a WAV stored raw at the start of a flash partition. */
esp_err_t aud_player_play_partition(const esp_partition_t *partition);

#endif
