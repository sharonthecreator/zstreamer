/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsrc_spi

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

#if defined(CONFIG_SPI_ASYNC)
#include "spi_dma_context.h"
#endif

LOG_MODULE_REGISTER(zstsrc_spi, CONFIG_ZSTNODE_LOG_LEVEL);

#if defined(CONFIG_SPI_ASYNC)
#ifndef CONFIG_ZSTNODE_SPI_DMA_RX_BUF_SIZE
#define CONFIG_ZSTNODE_SPI_DMA_RX_BUF_SIZE 256
#endif
#endif

struct zstsrc_spi_config {
	struct zstnode_common_config common;
	const struct device *spi_dev;
	struct spi_config spi_cfg;
	size_t rx_length;
};

struct zstsrc_spi_data {
	struct zstnode_common_data common;
#if defined(CONFIG_SPI_ASYNC)
	uint8_t dma_rx_buf[CONFIG_ZSTNODE_SPI_DMA_RX_BUF_SIZE];
	struct k_sem rx_sem;
	int rx_result;
	bool dma_enabled;
#endif
};

#if defined(CONFIG_SPI_ASYNC)

static void zstsrc_spi_rx_handler(void *user_data, int result)
{
	struct zstsrc_spi_data *data = user_data;

	data->rx_result = result;
	k_sem_give(&data->rx_sem);
}

static int zstsrc_spi_start_dma(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	struct zstsrc_spi_data *data = dev->data;
	int ret;

	ret = spi_dma_context_register_rx(cfg->spi_dev,
					  zstsrc_spi_rx_handler, data);
	if (ret != 0) {
		LOG_ERR("Failed to register SPI RX handler: %d", ret);
		return ret;
	}

	data->dma_enabled = true;
	LOG_DBG("SPI DMA RX enabled");
	return 0;
}

static int zstsrc_spi_stop_dma(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	struct zstsrc_spi_data *data = dev->data;

	if (data->dma_enabled) {
		spi_dma_context_unregister_rx(cfg->spi_dev);
		data->dma_enabled = false;
	}
	return 0;
}

static int zstsrc_spi_run_dma(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	struct zstsrc_spi_data *data = dev->data;
	struct net_buf *buf;
	size_t rx_len;
	int ret;

	rx_len = MIN(cfg->rx_length, sizeof(data->dma_rx_buf));

	struct spi_buf rx_buf = {
		.buf = data->dma_rx_buf,
		.len = rx_len,
	};
	struct spi_buf_set rx_bufs = {
		.buffers = &rx_buf,
		.count = 1,
	};

	ret = spi_dma_context_read_async(cfg->spi_dev, &cfg->spi_cfg, &rx_bufs);
	if (ret != 0) {
		LOG_ERR("SPI async read failed: %d", ret);
		return 0;
	}

	/* Wait for completion. */
	if (k_sem_take(&data->rx_sem, K_MSEC(1000)) != 0) {
		LOG_WRN("SPI RX timeout");
		return 0;
	}

	if (data->rx_result != 0) {
		LOG_ERR("SPI RX error: %d", data->rx_result);
		return 0;
	}

	buf = zstreamer_alloc_buf(dev, K_MSEC(100));
	if (buf == NULL) {
		LOG_WRN("Buffer alloc failed, data lost");
		return 0;
	}

	size_t copy_len = MIN(rx_len, net_buf_tailroom(buf));
	net_buf_add_mem(buf, data->dma_rx_buf, copy_len);

	return zstreamer_submit_buffer(dev, buf);
}

#endif /* CONFIG_SPI_ASYNC */

static int zstsrc_spi_run_poll(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	struct net_buf *buf;
	int ret;

	buf = zstreamer_alloc_buf(dev, K_MSEC(100));
	if (buf == NULL) {
		return 0;
	}

	size_t rx_len = MIN(cfg->rx_length, net_buf_tailroom(buf));

	struct spi_buf rx_buf = {
		.buf = buf->data,
		.len = rx_len,
	};
	struct spi_buf_set rx_bufs = {
		.buffers = &rx_buf,
		.count = 1,
	};

	ret = spi_read(cfg->spi_dev, &cfg->spi_cfg, &rx_bufs);
	if (ret != 0) {
		LOG_ERR("SPI read failed: %d", ret);
		net_buf_unref(buf);
		k_msleep(1);
		return 0;
	}

	net_buf_add(buf, rx_len);
	return zstreamer_submit_buffer(dev, buf);
}

static int zstsrc_spi_run(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	struct zstsrc_spi_data *data = dev->data;

	if (data->dma_enabled) {
		return zstsrc_spi_run_dma(dev);
	}
#endif
	return zstsrc_spi_run_poll(dev);
}

#if defined(CONFIG_SPI_ASYNC)
static int zstsrc_spi_start(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	int ret;

	ret = zstsrc_spi_start_dma(dev);
	if (ret != 0) {
		LOG_INF("SPI DMA not available for %s, using polling",
			cfg->spi_dev->name);
	}
	return 0;
}

static int zstsrc_spi_stop(const struct device *dev)
{
	return zstsrc_spi_stop_dma(dev);
}
#endif

static const struct zstnode_driver_api zstsrc_spi_api = {
#if defined(CONFIG_SPI_ASYNC)
	.start = zstsrc_spi_start,
	.stop = zstsrc_spi_stop,
#endif
	.run = zstsrc_spi_run,
};

#if defined(CONFIG_SPI_ASYNC)
static int zstsrc_spi_init(const struct device *dev)
{
	struct zstsrc_spi_data *data = dev->data;

	k_sem_init(&data->rx_sem, 0, 1);
	return 0;
}
#define ZSTSRC_SPI_INIT_FN zstsrc_spi_init
#else
#define ZSTSRC_SPI_INIT_FN NULL
#endif

/* Build SPI config from devicetree properties. */
#define ZSTSRC_SPI_CONFIG_FLAGS(inst)                                          \
	(DT_INST_PROP(inst, spi_cpol) ? SPI_MODE_CPOL : 0) |                    \
	(DT_INST_PROP(inst, spi_cpha) ? SPI_MODE_CPHA : 0)

#define ZSTSRC_SPI_DEFINE(inst)                                                \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct zstsrc_spi_data zstsrc_spi_data_##inst = {               \
		.common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                     \
			zstnode_stack_##inst),                                  \
	};                                                                     \
	static const struct zstsrc_spi_config zstsrc_spi_config_##inst = {     \
		.common = Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                   \
			DT_DRV_INST(inst),                                     \
			ZSTNODE_TYPE_SOURCE,                                   \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)),                  \
		.spi_dev = DEVICE_DT_GET(                                      \
			DT_INST_PHANDLE(inst, spi_device)),                    \
		.spi_cfg = {                                                   \
			.frequency = DT_INST_PROP(inst, spi_max_frequency),    \
			.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |    \
				     ZSTSRC_SPI_CONFIG_FLAGS(inst),             \
		},                                                             \
		.rx_length = DT_INST_PROP(inst, rx_length),                    \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, ZSTSRC_SPI_INIT_FN)                \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsrc_spi_data_##inst,                                       \
		&zstsrc_spi_config_##inst,                                     \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsrc_spi_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSRC_SPI_DEFINE)
