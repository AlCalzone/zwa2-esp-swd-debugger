idf_component_register(
    SRCS "main.c" "swd.c" "zg23.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio console esp_timer)
