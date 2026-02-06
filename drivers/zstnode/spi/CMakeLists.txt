# Copyright (c) 2026 zstreamer contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTNODE_SPI_DMA spi_dma_context.c)
zephyr_library_sources_ifdef(CONFIG_ZSTNODE_SPI_SRC zstsrc_spi.c)
zephyr_library_sources_ifdef(CONFIG_ZSTNODE_SPI_SINK zstsink_spi.c)
