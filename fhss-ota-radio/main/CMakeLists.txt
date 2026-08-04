idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_update bootloader_support esp_partition
                             spi_flash esp_driver_usb_serial_jtag)
