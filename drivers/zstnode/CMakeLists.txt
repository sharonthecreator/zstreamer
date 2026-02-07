# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

if(CONFIG_ZSTNODE_UART_SRC OR CONFIG_ZSTNODE_UART_SINK)
  add_subdirectory(uart)
endif()

if(CONFIG_ZSTNODE_SPI_SRC OR CONFIG_ZSTNODE_SPI_SINK)
  add_subdirectory(spi)
endif()

if(CONFIG_ZSTNODE_ADC_SRC)
  add_subdirectory(adc)
endif()

if(CONFIG_ZSTNODE_FS_SINK)
  add_subdirectory(fs)
endif()

if(CONFIG_ZSTNODE_TEST_NUMGEN_SRC OR CONFIG_ZSTNODE_TEST_FAKESINK)
  add_subdirectory(test)
endif()
