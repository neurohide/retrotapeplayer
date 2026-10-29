idf_component_register(SRCS "tap.c" "tzx.c" "cas.c" "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_spi esp_driver_gpio esp_driver_dac
                                esp_driver_i2c
                                fatfs sdmmc esp_rom json esp_app_format
                       EMBED_FILES "cassette-tdk-d46.bin" "cassette-sony-hf60.bin" "splash.bin")

# Bundle sdcard/ into a read-only flash library, used when no microSD card mounts.
fatfs_create_rawflash_image(storage ../sdcard FLASH_IN_PROJECT)
