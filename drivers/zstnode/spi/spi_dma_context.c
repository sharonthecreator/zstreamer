/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include "spi_dma_context.h"

LOG_MODULE_REGISTER(spi_dma_context, CONFIG_ZSTNODE_LOG_LEVEL);

#ifndef CONFIG_ZSTNODE_SPI_DMA_MAX_CONTEXTS
#define CONFIG_ZSTNODE_SPI_DMA_MAX_CONTEXTS 4
#endif

struct spi_dma_context {
	const struct device *spi_dev;

	/* RX side */
	spi_dma_handler_t rx_handler;
	void *rx_user_data;

	/* TX side */
	spi_dma_handler_t tx_handler;
	void *tx_user_data;

	/* Track which operation is in progress */
	bool rx_pending;
	bool tx_pending;
};

static struct spi_dma_context contexts[CONFIG_ZSTNODE_SPI_DMA_MAX_CONTEXTS];
static struct k_spinlock lock;

/**
 * Find or create a context for the given SPI device.
 * Must be called with lock held.
 */
static struct spi_dma_context *find_or_create_context(
	const struct device *spi_dev)
{
	struct spi_dma_context *free_slot = NULL;

	for (int i = 0; i < CONFIG_ZSTNODE_SPI_DMA_MAX_CONTEXTS; i++) {
		if (contexts[i].spi_dev == spi_dev) {
			return &contexts[i];
		}
		if (contexts[i].spi_dev == NULL && free_slot == NULL) {
			free_slot = &contexts[i];
		}
	}

	if (free_slot != NULL) {
		free_slot->spi_dev = spi_dev;
	}

	return free_slot;
}

/**
 * Find existing context for the given SPI device.
 * Must be called with lock held.
 */
static struct spi_dma_context *find_context(const struct device *spi_dev)
{
	for (int i = 0; i < CONFIG_ZSTNODE_SPI_DMA_MAX_CONTEXTS; i++) {
		if (contexts[i].spi_dev == spi_dev) {
			return &contexts[i];
		}
	}
	return NULL;
}

/**
 * SPI async callback that dispatches to registered handlers.
 * Note: Zephyr SPI async uses k_poll_signal, not direct callbacks.
 * This is kept for potential future use with alternative async mechanisms.
 */
static void __maybe_unused spi_dma_callback(const struct device *spi_dev,
					    int result, void *user_data)
{
	ARG_UNUSED(spi_dev);

	struct spi_dma_context *ctx = user_data;

	/* Dispatch based on which operation was pending. */
	if (ctx->rx_pending && ctx->rx_handler != NULL) {
		ctx->rx_pending = false;
		ctx->rx_handler(ctx->rx_user_data, result);
	}

	if (ctx->tx_pending && ctx->tx_handler != NULL) {
		ctx->tx_pending = false;
		ctx->tx_handler(ctx->tx_user_data, result);
	}
}

int spi_dma_context_register_rx(const struct device *spi_dev,
				spi_dma_handler_t handler,
				void *user_data)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_or_create_context(spi_dev);
	int ret = 0;

	if (ctx == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (ctx->rx_handler != NULL) {
		ret = -EBUSY;
		goto out;
	}

	ctx->rx_handler = handler;
	ctx->rx_user_data = user_data;

out:
	k_spin_unlock(&lock, key);
	return ret;
}

void spi_dma_context_unregister_rx(const struct device *spi_dev)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_context(spi_dev);

	if (ctx != NULL) {
		ctx->rx_handler = NULL;
		ctx->rx_user_data = NULL;

		if (ctx->tx_handler == NULL) {
			ctx->spi_dev = NULL;
		}
	}

	k_spin_unlock(&lock, key);
}

int spi_dma_context_register_tx(const struct device *spi_dev,
				spi_dma_handler_t handler,
				void *user_data)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_or_create_context(spi_dev);
	int ret = 0;

	if (ctx == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (ctx->tx_handler != NULL) {
		ret = -EBUSY;
		goto out;
	}

	ctx->tx_handler = handler;
	ctx->tx_user_data = user_data;

out:
	k_spin_unlock(&lock, key);
	return ret;
}

void spi_dma_context_unregister_tx(const struct device *spi_dev)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_context(spi_dev);

	if (ctx != NULL) {
		ctx->tx_handler = NULL;
		ctx->tx_user_data = NULL;

		if (ctx->rx_handler == NULL) {
			ctx->spi_dev = NULL;
		}
	}

	k_spin_unlock(&lock, key);
}

int spi_dma_context_read_async(const struct device *spi_dev,
			       const struct spi_config *config,
			       const struct spi_buf_set *rx_bufs)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_context(spi_dev);
	int ret;

	if (ctx == NULL) {
		k_spin_unlock(&lock, key);
		return -EINVAL;
	}

	ctx->rx_pending = true;
	k_spin_unlock(&lock, key);

	ret = spi_read_signal(spi_dev, config, rx_bufs, NULL);
	if (ret != 0) {
		key = k_spin_lock(&lock);
		ctx->rx_pending = false;
		k_spin_unlock(&lock, key);
	}

	return ret;
}

int spi_dma_context_write_async(const struct device *spi_dev,
				const struct spi_config *config,
				const struct spi_buf_set *tx_bufs)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_context(spi_dev);
	int ret;

	if (ctx == NULL) {
		k_spin_unlock(&lock, key);
		return -EINVAL;
	}

	ctx->tx_pending = true;
	k_spin_unlock(&lock, key);

	ret = spi_write_signal(spi_dev, config, tx_bufs, NULL);
	if (ret != 0) {
		key = k_spin_lock(&lock);
		ctx->tx_pending = false;
		k_spin_unlock(&lock, key);
	}

	return ret;
}

int spi_dma_context_transceive_async(const struct device *spi_dev,
				     const struct spi_config *config,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct spi_dma_context *ctx = find_context(spi_dev);
	int ret;

	if (ctx == NULL) {
		k_spin_unlock(&lock, key);
		return -EINVAL;
	}

	ctx->tx_pending = true;
	ctx->rx_pending = true;
	k_spin_unlock(&lock, key);

	ret = spi_transceive_signal(spi_dev, config, tx_bufs, rx_bufs, NULL);
	if (ret != 0) {
		key = k_spin_lock(&lock);
		ctx->tx_pending = false;
		ctx->rx_pending = false;
		k_spin_unlock(&lock, key);
	}

	return ret;
}
