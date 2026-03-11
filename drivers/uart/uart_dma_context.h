/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief UART DMA context for sharing async callbacks between src/sink nodes
 *
 * Zephyr's uart_callback_set() only allows one callback per UART device.
 * This module provides a shared context so both src-uart (RX) and
 * sink-uart (TX) can use async/DMA on the same UART.
 */

#ifndef ZSTREAMER_UART_DMA_CONTEXT_H_
#define ZSTREAMER_UART_DMA_CONTEXT_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RX event handler callback type.
 *
 * @param user_data  User data provided at registration.
 * @param data       Pointer to received data.
 * @param len        Length of received data.
 */
typedef void (*uart_dma_rx_handler_t)(void *user_data, const uint8_t *data, size_t len);

/**
 * @brief TX complete handler callback type.
 *
 * @param user_data  User data provided at registration.
 * @param err        0 on success, negative errno on error.
 */
typedef void (*uart_dma_tx_handler_t)(void *user_data, int err);

/**
 * @brief Register an RX handler for a UART device.
 *
 * Only one RX handler can be registered per UART device.
 *
 * @param uart_dev   UART device.
 * @param handler    RX data handler callback.
 * @param user_data  User data passed to handler.
 * @return 0 on success, -EBUSY if already registered, -ENOMEM if no slots.
 */
int uart_dma_context_register_rx(const struct device *uart_dev, uart_dma_rx_handler_t handler,
				 void *user_data);

/**
 * @brief Unregister the RX handler for a UART device.
 *
 * @param uart_dev  UART device.
 */
void uart_dma_context_unregister_rx(const struct device *uart_dev);

/**
 * @brief Register a TX handler for a UART device.
 *
 * Only one TX handler can be registered per UART device.
 *
 * @param uart_dev   UART device.
 * @param handler    TX complete handler callback.
 * @param user_data  User data passed to handler.
 * @return 0 on success, -EBUSY if already registered, -ENOMEM if no slots.
 */
int uart_dma_context_register_tx(const struct device *uart_dev, uart_dma_tx_handler_t handler,
				 void *user_data);

/**
 * @brief Unregister the TX handler for a UART device.
 *
 * @param uart_dev  UART device.
 */
void uart_dma_context_unregister_tx(const struct device *uart_dev);

/**
 * @brief Enable RX on a UART device with DMA.
 *
 * @param uart_dev  UART device.
 * @param buf       Buffer for receiving data.
 * @param len       Buffer length.
 * @param timeout   Timeout in microseconds (0 = no timeout).
 * @return 0 on success, negative errno on failure.
 */
int uart_dma_context_rx_enable(const struct device *uart_dev, uint8_t *buf, size_t len,
			       int32_t timeout);

/**
 * @brief Disable RX on a UART device.
 *
 * @param uart_dev  UART device.
 * @return 0 on success, negative errno on failure.
 */
int uart_dma_context_rx_disable(const struct device *uart_dev);

/**
 * @brief Transmit data via DMA.
 *
 * Non-blocking. TX handler is called on completion.
 *
 * @param uart_dev  UART device.
 * @param data      Data to transmit.
 * @param len       Data length.
 * @return 0 on success, negative errno on failure.
 */
int uart_dma_context_tx(const struct device *uart_dev, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_UART_DMA_CONTEXT_H_ */
