/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include "uart_dma_context.h"

LOG_MODULE_REGISTER(uart_dma_context, CONFIG_ZSTNODE_LOG_LEVEL);

#ifndef CONFIG_ZSTNODE_UART_DMA_MAX_CONTEXTS
#define CONFIG_ZSTNODE_UART_DMA_MAX_CONTEXTS 4
#endif

struct uart_dma_context {
	const struct device *uart_dev;

	/* RX side */
	uart_dma_rx_handler_t rx_handler;
	void *rx_user_data;

	/* TX side */
	uart_dma_tx_handler_t tx_handler;
	void *tx_user_data;
};

static struct uart_dma_context contexts[CONFIG_ZSTNODE_UART_DMA_MAX_CONTEXTS];
static struct k_spinlock lock;

/**
 * Find or create a context for the given UART device.
 * Must be called with lock held.
 */
static struct uart_dma_context *find_or_create_context(
	const struct device *uart_dev)
{
	struct uart_dma_context *free_slot = NULL;

	for (int i = 0; i < CONFIG_ZSTNODE_UART_DMA_MAX_CONTEXTS; i++) {
		if (contexts[i].uart_dev == uart_dev) {
			return &contexts[i];
		}
		if (contexts[i].uart_dev == NULL && free_slot == NULL) {
			free_slot = &contexts[i];
		}
	}

	if (free_slot != NULL) {
		free_slot->uart_dev = uart_dev;
	}

	return free_slot;
}

/**
 * Find existing context for the given UART device.
 * Must be called with lock held.
 */
static struct uart_dma_context *find_context(const struct device *uart_dev)
{
	for (int i = 0; i < CONFIG_ZSTNODE_UART_DMA_MAX_CONTEXTS; i++) {
		if (contexts[i].uart_dev == uart_dev) {
			return &contexts[i];
		}
	}
	return NULL;
}

/**
 * Master UART callback that dispatches to registered RX/TX handlers.
 */
static void uart_dma_callback(const struct device *uart_dev,
			      struct uart_event *evt, void *user_data)
{
	struct uart_dma_context *ctx = user_data;

	switch (evt->type) {
	case UART_RX_RDY:
		if (ctx->rx_handler != NULL) {
			ctx->rx_handler(ctx->rx_user_data,
					evt->data.rx.buf + evt->data.rx.offset,
					evt->data.rx.len);
		}
		break;

	case UART_RX_BUF_REQUEST:
		/* For continuous reception, the caller should manage buffers.
		 * This event indicates the driver needs another buffer.
		 * For now we don't handle double-buffering here.
		 */
		break;

	case UART_RX_BUF_RELEASED:
		/* Buffer is no longer in use by the driver. */
		break;

	case UART_RX_DISABLED:
		LOG_DBG("UART RX disabled");
		break;

	case UART_RX_STOPPED:
		LOG_WRN("UART RX stopped due to error");
		break;

	case UART_TX_DONE:
		if (ctx->tx_handler != NULL) {
			ctx->tx_handler(ctx->tx_user_data, 0);
		}
		break;

	case UART_TX_ABORTED:
		if (ctx->tx_handler != NULL) {
			ctx->tx_handler(ctx->tx_user_data, -ECANCELED);
		}
		break;

	default:
		break;
	}
}

int uart_dma_context_register_rx(const struct device *uart_dev,
				 uart_dma_rx_handler_t handler,
				 void *user_data)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct uart_dma_context *ctx = find_or_create_context(uart_dev);
	int ret = 0;

	if (ctx == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (ctx->rx_handler != NULL) {
		ret = -EBUSY;
		goto out;
	}

	/* Set up the UART callback if this is the first handler. */
	if (ctx->rx_handler == NULL && ctx->tx_handler == NULL) {
		ret = uart_callback_set(uart_dev, uart_dma_callback, ctx);
		if (ret != 0) {
			LOG_ERR("Failed to set UART callback: %d", ret);
			goto out;
		}
	}

	ctx->rx_handler = handler;
	ctx->rx_user_data = user_data;

out:
	k_spin_unlock(&lock, key);
	return ret;
}

void uart_dma_context_unregister_rx(const struct device *uart_dev)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct uart_dma_context *ctx = find_context(uart_dev);

	if (ctx != NULL) {
		ctx->rx_handler = NULL;
		ctx->rx_user_data = NULL;

		/* Clear context if no handlers remain. */
		if (ctx->tx_handler == NULL) {
			ctx->uart_dev = NULL;
		}
	}

	k_spin_unlock(&lock, key);
}

int uart_dma_context_register_tx(const struct device *uart_dev,
				 uart_dma_tx_handler_t handler,
				 void *user_data)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct uart_dma_context *ctx = find_or_create_context(uart_dev);
	int ret = 0;

	if (ctx == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (ctx->tx_handler != NULL) {
		ret = -EBUSY;
		goto out;
	}

	/* Set up the UART callback if this is the first handler. */
	if (ctx->rx_handler == NULL && ctx->tx_handler == NULL) {
		ret = uart_callback_set(uart_dev, uart_dma_callback, ctx);
		if (ret != 0) {
			LOG_ERR("Failed to set UART callback: %d", ret);
			goto out;
		}
	}

	ctx->tx_handler = handler;
	ctx->tx_user_data = user_data;

out:
	k_spin_unlock(&lock, key);
	return ret;
}

void uart_dma_context_unregister_tx(const struct device *uart_dev)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct uart_dma_context *ctx = find_context(uart_dev);

	if (ctx != NULL) {
		ctx->tx_handler = NULL;
		ctx->tx_user_data = NULL;

		/* Clear context if no handlers remain. */
		if (ctx->rx_handler == NULL) {
			ctx->uart_dev = NULL;
		}
	}

	k_spin_unlock(&lock, key);
}

int uart_dma_context_rx_enable(const struct device *uart_dev, uint8_t *buf,
			       size_t len, int32_t timeout)
{
	return uart_rx_enable(uart_dev, buf, len, timeout);
}

int uart_dma_context_rx_disable(const struct device *uart_dev)
{
	return uart_rx_disable(uart_dev);
}

int uart_dma_context_tx(const struct device *uart_dev, const uint8_t *data,
			size_t len)
{
	return uart_tx(uart_dev, data, len, SYS_FOREVER_US);
}
