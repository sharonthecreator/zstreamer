# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_ADC_SRC adc_src.c)
zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_ADC_DMA_STM32_SRC adc_dma_stm32_src.c)
