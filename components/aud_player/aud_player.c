#include "aud_player.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "driver/dac_oneshot.h"


#define TAG "AUD_PLAYER"

#define SAMPLE_DELAY_US 62


static dac_oneshot_handle_t dac_handle;


esp_err_t aud_player_play(const char *filename)
{
    ESP_LOGI(TAG, "Playing %s", filename);


    FILE *file = fopen(filename, "rb");

    if(file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open WAV");
        return ESP_FAIL;
    }


    // Skip WAV header
    fseek(file, 44, SEEK_SET);



    // GPIO25 DAC
    dac_oneshot_config_t dac_config =
    {
        .chan_id = DAC_CHAN_0
    };


    ESP_ERROR_CHECK(
        dac_oneshot_new_channel(
            &dac_config,
            &dac_handle
        )
    );


    uint8_t sample;


    while(fread(&sample,1,1,file)==1)
    {

       ESP_ERROR_CHECK(
    dac_oneshot_output_voltage(
        dac_handle,
        sample
    )
);


        // 16000Hz
        esp_rom_delay_us(
            SAMPLE_DELAY_US
        );
    }



    fclose(file);


    dac_oneshot_del_channel(
        dac_handle
    );


    ESP_LOGI(TAG, "Playback completed");


    return ESP_OK;
}