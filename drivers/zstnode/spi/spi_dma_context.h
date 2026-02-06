/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SPI DMA context for sharing async callbacks between src/sink nodes
 *
 * Similar to UART DMA context, this module provides a shared context so
 * both zstsrc-spi (RX) and zstsink-spi (TX) can use async/DMA on the
 * same SPI bus.
 */

#ifndef ZSTNODE_SPI_DMA_CONTEXT_H_
#define ZSTNODE_SPI_DMA_CONTEXT_H_

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SPI async completion handler callback type.
 *
 * @param user_data  User data provided at registration.
 * @param result     0 on success, negative errno on error.
 */
typedef void (*spi_dma_handler_t)(void *user_data, int result);

/**
 * @brief Register an RX handler for an SPI device.
 *
 * @param spi_dev    SPI device.
 * @param handler    Completion handler callback.
 * @param user_data  User data passed to handler.
 * @return 0 on success, -EBUSY if already registered, -ENOMEM if no slots.
 */
int spi_dma_context_register_rx(const struct device *spi_dev,
				spi_dma_handler_t handler,
				void *user_data);

/**
 * @brief Unregister the RX handler for an SPI device.
 *
 * @param spi_dev  SPI device.
 */
void spi_dma_context_unregister_rx(const struct device *spi_dev);

/**
 * @brief Register a TX handler for an SPI device.
 *
 * @param spi_dev    SPI device.
 * @param handler    Completion handler callback.
 * @param user_data  User data passed to handler.
 * @return 0 on success, -EBUSY if already registered, -ENOMEM if no slots.
 */
int spi_dma_context_register_tx(const struct device *spi_dev,
				spi_dma_handler_t handler,
				void *user_data);

/**
 * @brief Unregister the TX handler for an SPI device.
 *
 * @param spi_dev  SPI device.
 */
void spi_dma_context_unregister_tx(const struct device *spi_dev);

/**
 * @brief Perform async SPI read (RX only).
 *
 * @param spi_dev  SPI device.
 * @param config   SPI configuration.
 * @param rx_bufs  Receive buffer set.
 * @return 0 on success (callback will be invoked), negative errno on failure.
 */
int spi_dma_context_read_async(const struct device *spi_dev,
			       const struct spi_config *config,
			       const struct spi_buf_set *rx_bufs);

/**
 * @brief Perform async SPI write (TX only).
 *
 * @param spi_dev  SPI device.
 * @param config   SPI configuration.
 * @param tx_bufs  Transmit buffer set.
 * @return 0 on success (callback will be invoked), negative errno on failure.
 */
int spi_dma_context_write_async(const struct device *spi_dev,
				const struct spi_config *config,
				const struct spi_buf_set *tx_bufs);

/**
 * @brief Perform async SPI transceive (TX and RX).
 *
 * @param spi_dev  SPI device.
 * @param config   SPI configuration.
 * @param tx_bufs  Transmit buffer set.
 * @param rx_bufs  Receive buffer set.
 * @return 0 on success (callback will be invoked), negative errno on failure.
 */
int spi_dma_context_transceive_async(const struct device *spi_dev,
				     const struct spi_config *config,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs);

#ifdef __cplusplus
}
#endif

#endif /* ZSTNODE_SPI_DMA_CONTEXT_H_ */
