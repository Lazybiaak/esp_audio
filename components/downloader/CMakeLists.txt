idf_component_register(
    SRCS "downloader.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp-tls
        mbedtls
        esp_partition
)
