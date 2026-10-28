idf_component_register(SRCS "tap.c" "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_spi esp_driver_gpio esp_driver_dac
                                fatfs sdmmc esp_rom)