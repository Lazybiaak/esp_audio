idf_component_register(
    SRCS "filesystem.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES joltwallet__littlefs
)