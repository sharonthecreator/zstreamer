/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsink_spi

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

#if defined(CONFIG_SPI_ASYNC)
#include "spi_dma_context.h"
#endif

LOG_MODULE_REGISTER(zstsink_spi, CONFIG_ZSTNODE_LOG_LEVEL);

struct zstsink_spi_config {
	struct zstnode_common_config common;
	const struct device *spi_dev;
	struct spi_config spi_cfg;
};

struct zstsink_spi_data {
	struct zstnode_common_data common;
#if defined(CONFIG_SPI_ASYNC)
	struct k_sem tx_sem;
	int tx_result;
	bool dma_enabled;
#endif
};

#if defined(CONFIG_SPI_ASYNC)

static void zstsink_spi_tx_handler(void *user_data, int result)
{
	struct zstsink_spi_data *data = user_data;

	data->tx_result = result;
	k_sem_give(&data->tx_sem);
}

static int zstsink_spi_start_dma(const struct device *dev)
{
	const struct zstsink_spi_config *cfg = dev->config;
	struct zstsink_spi_data *data = dev->data;
	int ret;

	ret = spi_dma_context_register_tx(cfg->spi_dev,
					  zstsink_spi_tx_handler, data);
	if (ret != 0) {
		LOG_ERR("Failed to register SPI TX handler: %d", ret);
		return ret;
	}

	data->dma_enabled = true;
	LOG_DBG("SPI DMA TX enabled");
	return 0;
}

static int zstsink_spi_stop_dma(const struct device *dev)
{
	const struct zstsink_spi_config *cfg = dev->config;
	struct zstsink_spi_data *data = dev->data;

	if (data->dma_enabled) {
		spi_dma_context_unregister_tx(cfg->spi_dev);
		data->dma_enabled = false;
	}
	return 0;
}

static int zstsink_spi_process_dma(const struct device *dev,
				   struct net_buf *buf)
{
	const struct zstsink_spi_config *cfg = dev->config;
	struct zstsink_spi_data *data = dev->data;
	int ret;

	if (buf->len == 0) {
		return 0;
	}

	struct spi_buf tx_buf = {
		.buf = buf->data,
		.len = buf->len,
	};
	struct spi_buf_set tx_bufs = {
		.buffers = &tx_buf,
		.count = 1,
	};

	ret = spi_dma_context_write_async(cfg->spi_dev, &cfg->spi_cfg, &tx_bufs);
	if (ret != 0) {
		LOG_ERR("SPI async write failed: %d", ret);
		return ret;
	}

	/* Wait for completion. */
	k_sem_take(&data->tx_sem, K_FOREVER);

	return data->tx_result;
}

#endif /* CONFIG_SPI_ASYNC */

static int zstsink_spi_process_poll(const struct device *dev,
				    struct net_buf *buf)
{
	const struct zstsink_spi_config *cfg = dev->config;

	if (buf->len == 0) {
		return 0;
	}

	struct spi_buf tx_buf = {
		.buf = buf->data,
		.len = buf->len,
	};
	struct spi_buf_set tx_bufs = {
		.buffers = &tx_buf,
		.count = 1,
	};

	return spi_write(cfg->spi_dev, &cfg->spi_cfg, &tx_bufs);
}

static int zstsink_spi_process(const struct device *dev,
			       struct net_buf *buf)
{
#if defined(CONFIG_SPI_ASYNC)
	struct zstsink_spi_data *data = dev->data;

	if (data->dma_enabled) {
		return zstsink_spi_process_dma(dev, buf);
	}
#endif
	return zstsink_spi_process_poll(dev, buf);
}

#if defined(CONFIG_SPI_ASYNC)
static int zstsink_spi_start(const struct device *dev)
{
	const struct zstsink_spi_config *cfg = dev->config;
	int ret;

	ret = zstsink_spi_start_dma(dev);
	if (ret != 0) {
		LOG_INF("SPI DMA not available for %s, using polling",
			cfg->spi_dev->name);
	}
	return 0;
}

static int zstsink_spi_stop(const struct device *dev)
{
	return zstsink_spi_stop_dma(dev);
}
#endif

static const struct zstnode_driver_api zstsink_spi_api = {
#if defined(CONFIG_SPI_ASYNC)
	.start = zstsink_spi_start,
	.stop = zstsink_spi_stop,
#endif
	.process = zstsink_spi_process,
};

#if defined(CONFIG_SPI_ASYNC)
static int zstsink_spi_init(const struct device *dev)
{
	struct zstsink_spi_data *data = dev->data;

	k_sem_init(&data->tx_sem, 0, 1);
	return 0;
}
#define ZSTSINK_SPI_INIT_FN zstsink_spi_init
#else
#define ZSTSINK_SPI_INIT_FN NULL
#endif

/* Build SPI config from devicetree properties. */
#define ZSTSINK_SPI_CONFIG_FLAGS(inst)                                         \
	(DT_INST_PROP(inst, spi_cpol) ? SPI_MODE_CPOL : 0) |                    \
	(DT_INST_PROP(inst, spi_cpha) ? SPI_MODE_CPHA : 0)

#define ZSTSINK_SPI_DEFINE(inst)                                               \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct zstsink_spi_data zstsink_spi_data_##inst = {             \
		.common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                     \
			zstnode_stack_##inst),                                  \
	};                                                                     \
	static const struct zstsink_spi_config zstsink_spi_config_##inst = {   \
		.common = Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                   \
			DT_DRV_INST(inst),                                     \
			ZSTNODE_TYPE_SINK,                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)),                  \
		.spi_dev = DEVICE_DT_GET(                                      \
			DT_INST_PHANDLE(inst, spi_device)),                    \
		.spi_cfg = {                                                   \
			.frequency = DT_INST_PROP(inst, spi_max_frequency),    \
			.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |    \
				     ZSTSINK_SPI_CONFIG_FLAGS(inst),            \
		},                                                             \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, ZSTSINK_SPI_INIT_FN)               \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsink_spi_data_##inst,                                      \
		&zstsink_spi_config_##inst,                                    \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsink_spi_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSINK_SPI_DEFINE)
