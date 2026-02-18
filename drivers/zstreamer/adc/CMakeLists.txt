# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_ADC_SRC adc_src.c)

# STM32-specific implementation using LL drivers
zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_ADC_STM32 adc_src_stm32.c)
