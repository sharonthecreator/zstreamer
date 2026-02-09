/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_src_spi

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(src_spi, CONFIG_ZSTREAMER_NODE_LOG_LEVEL);

#ifndef CONFIG_ZSTREAMER_NODE_SPI_DMA_RX_BUF_SIZE
#define CONFIG_ZSTREAMER_NODE_SPI_DMA_RX_BUF_SIZE 256
#endif

struct src_spi_config {
	struct zstreamer_node_config common;
	struct spi_dt_spec spi;
	size_t rx_length;
};

struct src_spi_data {
	struct zstreamer_node_data common;
#if defined(CONFIG_SPI_ASYNC)
	uint8_t dma_rx_buf[CONFIG_ZSTREAMER_NODE_SPI_DMA_RX_BUF_SIZE];
	struct k_poll_signal sig;
	struct k_poll_event evt;
	bool async_enabled;
#endif
};

#if defined(CONFIG_SPI_ASYNC)

static int src_spi_process_async(const struct device *dev,
				    struct net_buf *buf)
{
	const struct src_spi_config *cfg = dev->config;
	struct src_spi_data *data = dev->data;
	int ret, result;
	size_t rx_len;

	rx_len = MIN(cfg->rx_length, sizeof(data->dma_rx_buf));

	struct spi_buf rx_buf = {
		.buf = data->dma_rx_buf,
		.len = rx_len,
	};
	struct spi_buf_set rx_bufs = {
		.buffers = &rx_buf,
		.count = 1,
	};

	k_poll_signal_reset(&data->sig);
	data->evt.state = K_POLL_STATE_NOT_READY;

	ret = spi_read_signal(cfg->spi.bus, &cfg->spi.config,
			      &rx_bufs, &data->sig);
	if (ret < 0) {
		LOG_ERR("SPI async read failed: %d", ret);
		return ret;
	}

	ret = k_poll(&data->evt, 1, K_MSEC(1000));
	if (ret < 0) {
		LOG_WRN("SPI RX poll timeout");
		return -EAGAIN;
	}

	result = data->sig.result;
	if (result < 0) {
		LOG_ERR("SPI RX error: %d", result);
		return result;
	}

	size_t copy_len = MIN(rx_len, net_buf_tailroom(buf));

	net_buf_add_mem(buf, data->dma_rx_buf, copy_len);

	return 0;
}

static int src_spi_open_async(const struct device *dev)
{
	const struct src_spi_config *cfg = dev->config;
	struct src_spi_data *data = dev->data;
	uint8_t dummy;
	struct spi_buf test_buf = { .buf = &dummy, .len = 1 };
	struct spi_buf_set test_bufs = { .buffers = &test_buf, .count = 1 };
	int ret;

	k_poll_signal_reset(&data->sig);
	data->evt.state = K_POLL_STATE_NOT_READY;

	ret = spi_read_signal(cfg->spi.bus, &cfg->spi.config,
			      &test_bufs, &data->sig);
	if (ret == -ENOTSUP) {
		LOG_INF("SPI async not supported, using polling");
		return -ENOTSUP;
	}
	if (ret < 0) {
		LOG_WRN("SPI async probe failed: %d, using polling", ret);
		return ret;
	}

	/* Wait for probe transfer to complete. */
	k_poll(&data->evt, 1, K_MSEC(100));

	data->async_enabled = true;
	LOG_DBG("SPI async RX enabled");
	return 0;
}

#endif /* CONFIG_SPI_ASYNC */

static int src_spi_process_poll(const struct device *dev,
				   struct net_buf *buf)
{
	const struct src_spi_config *cfg = dev->config;
	int ret;

	size_t rx_len = MIN(cfg->rx_length, net_buf_tailroom(buf));

	struct spi_buf rx_buf = {
		.buf = buf->data,
		.len = rx_len,
	};
	struct spi_buf_set rx_bufs = {
		.buffers = &rx_buf,
		.count = 1,
	};

	ret = spi_read_dt(&cfg->spi, &rx_bufs);
	if (ret < 0) {
		LOG_ERR("SPI read failed: %d", ret);
		k_msleep(1);
		return ret;
	}

	net_buf_add(buf, rx_len);

	/* On real hardware the SPI transaction itself takes time; on
	 * emulators it completes instantly.  Sleep briefly to avoid
	 * busy-looping and to let the simulated clock advance.
	 */
	k_msleep(1);

	return 0;
}

static int src_spi_process(const struct device *dev,
			      struct net_buf *buf)
{
#if defined(CONFIG_SPI_ASYNC)
	struct src_spi_data *data = dev->data;

	if (data->async_enabled) {
		return src_spi_process_async(dev, buf);
	}
#endif
	return src_spi_process_poll(dev, buf);
}

static int src_spi_open(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	src_spi_open_async(dev);
#endif
	return 0;
}

static int src_spi_close(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	struct src_spi_data *data = dev->data;

	data->async_enabled = false;
#endif
	return 0;
}

static const struct zstreamer_node_driver_api src_spi_api = {
	.open = src_spi_open,
	.close = src_spi_close,
	.generate = src_spi_process,
};

#if defined(CONFIG_SPI_ASYNC)
static int src_spi_init(const struct device *dev)
{
	struct src_spi_data *data = dev->data;

	k_poll_signal_init(&data->sig);
	k_poll_event_init(&data->evt, K_POLL_TYPE_SIGNAL,
			  K_POLL_MODE_NOTIFY_ONLY, &data->sig);
	return 0;
}
#define SRC_SPI_INIT_FN src_spi_init
#else
#define SRC_SPI_INIT_FN NULL
#endif

#define SPI_DEV_NODE(inst) DT_INST_PHANDLE(inst, spi_device)

#define SRC_SPI_DEFINE(inst)                                                \
	Z_ZSTREAMER_NODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct src_spi_data src_spi_data_##inst = {               \
		.common = Z_ZSTREAMER_NODE_DATA_INIT(inst,                     \
			zstreamer_node_stack_##inst),                                  \
	};                                                                     \
	static const struct src_spi_config src_spi_config_##inst = {     \
		.common = { Z_ZSTREAMER_NODE_CONFIG_INIT(inst,                 \
			DT_DRV_INST(inst),                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)) },                \
		.spi = SPI_DT_SPEC_GET(SPI_DEV_NODE(inst),                    \
				       SPI_OP_MODE_MASTER | SPI_WORD_SET(8),   \
				       0),                                     \
		.rx_length = DT_INST_PROP(inst, rx_length),                    \
	};                                                                     \
	Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, SRC_SPI_INIT_FN)                \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_node_init_##inst, NULL,                 \
		&src_spi_data_##inst,                                       \
		&src_spi_config_##inst,                                     \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&src_spi_api);

DT_INST_FOREACH_STATUS_OKAY(SRC_SPI_DEFINE)
