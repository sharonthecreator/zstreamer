# Copyright (c) 2026 zstreamer contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTNODE_ADC_SRC zstsrc_adc.c)

# STM32-specific implementation using LL drivers
zephyr_library_sources_ifdef(CONFIG_ZSTNODE_ADC_STM32 zstsrc_adc_stm32.c)
