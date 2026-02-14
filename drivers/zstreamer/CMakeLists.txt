# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

add_subdirectory_ifdef(CONFIG_ZSTREAMER_UART uart)
add_subdirectory_ifdef(CONFIG_ZSTREAMER_SPI spi)
add_subdirectory_ifdef(CONFIG_ZSTREAMER_ADC adc)
add_subdirectory_ifdef(CONFIG_ZSTREAMER_FS fs)
add_subdirectory_ifdef(CONFIG_ZSTREAMER_TEST test)
