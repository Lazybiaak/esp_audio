idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_partition
        wifi_manager
        downloader
        aud_player
)
