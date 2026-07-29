idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver
    filesystem
    wifi_manager
    downloader
    aud_player
)