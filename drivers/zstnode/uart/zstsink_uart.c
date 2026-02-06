/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsink_uart

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

#if defined(CONFIG_UART_ASYNC_API)
#include "uart_dma_context.h"
#endif

LOG_MODULE_REGISTER(zstsink_uart, CONFIG_ZSTNODE_LOG_LEVEL);

struct zstsink_uart_config {
	struct zstnode_common_config common;
	const struct device *uart_dev;
};

struct zstsink_uart_data {
	struct zstnode_common_data common;
#if defined(CONFIG_UART_ASYNC_API)
	struct k_sem tx_sem;
	int tx_err;
	bool dma_enabled;
#endif
};

#if defined(CONFIG_UART_ASYNC_API)

static void zstsink_uart_tx_handler(void *user_data, int err)
{
	struct zstsink_uart_data *data = user_data;

	data->tx_err = err;
	k_sem_give(&data->tx_sem);
}

static int zstsink_uart_start_dma(const struct device *dev)
{
	const struct zstsink_uart_config *cfg = dev->config;
	struct zstsink_uart_data *data = dev->data;
	int ret;

	ret = uart_dma_context_register_tx(cfg->uart_dev,
					   zstsink_uart_tx_handler, data);
	if (ret != 0) {
		LOG_ERR("Failed to register TX handler: %d", ret);
		return ret;
	}

	data->dma_enabled = true;
	LOG_DBG("DMA TX enabled");
	return 0;
}

static int zstsink_uart_stop_dma(const struct device *dev)
{
	const struct zstsink_uart_config *cfg = dev->config;
	struct zstsink_uart_data *data = dev->data;

	if (data->dma_enabled) {
		uart_dma_context_unregister_tx(cfg->uart_dev);
		data->dma_enabled = false;
	}
	return 0;
}

static int zstsink_uart_process_dma(const struct device *dev,
				    struct net_buf *buf)
{
	const struct zstsink_uart_config *cfg = dev->config;
	struct zstsink_uart_data *data = dev->data;
	int ret;

	if (buf->len == 0) {
		return 0;
	}

	ret = uart_dma_context_tx(cfg->uart_dev, buf->data, buf->len);
	if (ret != 0) {
		LOG_ERR("DMA TX failed: %d", ret);
		return ret;
	}

	/* Wait for TX completion. */
	k_sem_take(&data->tx_sem, K_FOREVER);

	return data->tx_err;
}

#endif /* CONFIG_UART_ASYNC_API */

static int zstsink_uart_process_poll(const struct device *dev,
				     struct net_buf *buf)
{
	const struct zstsink_uart_config *cfg = dev->config;

	for (uint16_t i = 0; i < buf->len; i++) {
		uart_poll_out(cfg->uart_dev, buf->data[i]);
	}

	return 0;
}

static int zstsink_uart_process(const struct device *dev,
				struct net_buf *buf)
{
#if defined(CONFIG_UART_ASYNC_API)
	struct zstsink_uart_data *data = dev->data;

	if (data->dma_enabled) {
		return zstsink_uart_process_dma(dev, buf);
	}
#endif
	return zstsink_uart_process_poll(dev, buf);
}

#if defined(CONFIG_UART_ASYNC_API)
static int zstsink_uart_start(const struct device *dev)
{
	const struct zstsink_uart_config *cfg = dev->config;
	int ret;

	/* Try to enable DMA, fall back to polling if it fails. */
	ret = zstsink_uart_start_dma(dev);
	if (ret != 0) {
		LOG_INF("DMA not available for %s, using polling",
			cfg->uart_dev->name);
	}
	return 0;
}

static int zstsink_uart_stop(const struct device *dev)
{
	return zstsink_uart_stop_dma(dev);
}
#endif

static const struct zstnode_driver_api zstsink_uart_api = {
#if defined(CONFIG_UART_ASYNC_API)
	.start = zstsink_uart_start,
	.stop = zstsink_uart_stop,
#endif
	.process = zstsink_uart_process,
};

#if defined(CONFIG_UART_ASYNC_API)
static int zstsink_uart_init(const struct device *dev)
{
	struct zstsink_uart_data *data = dev->data;

	k_sem_init(&data->tx_sem, 0, 1);
	return 0;
}
#define ZSTSINK_UART_INIT_FN zstsink_uart_init
#else
#define ZSTSINK_UART_INIT_FN NULL
#endif

#define ZSTSINK_UART_DEFINE(inst)                                              \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct zstsink_uart_data zstsink_uart_data_##inst = {           \
		.common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                     \
			zstnode_stack_##inst),                                  \
	};                                                                     \
	static const struct zstsink_uart_config zstsink_uart_config_##inst = { \
		.common = Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                   \
			DT_DRV_INST(inst),                                     \
			ZSTNODE_TYPE_SINK,                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)),                  \
		.uart_dev = DEVICE_DT_GET(                                     \
			DT_INST_PHANDLE(inst, uart_device)),                   \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, ZSTSINK_UART_INIT_FN)              \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsink_uart_data_##inst,                                     \
		&zstsink_uart_config_##inst,                                   \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsink_uart_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSINK_UART_DEFINE)
