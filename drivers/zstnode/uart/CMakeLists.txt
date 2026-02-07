# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTNODE_UART_DMA uart_dma_context.c)
zephyr_library_sources_ifdef(CONFIG_ZSTNODE_UART_SRC zstsrc_uart.c)
zephyr_library_sources_ifdef(CONFIG_ZSTNODE_UART_SINK zstsink_uart.c)
