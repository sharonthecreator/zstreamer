/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsrc_spi

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(zstsrc_spi, CONFIG_ZSTNODE_LOG_LEVEL);

#ifndef CONFIG_ZSTNODE_SPI_DMA_RX_BUF_SIZE
#define CONFIG_ZSTNODE_SPI_DMA_RX_BUF_SIZE 256
#endif

struct zstsrc_spi_config {
	struct zstnode_common_config common;
	struct spi_dt_spec spi;
	size_t rx_length;
};

struct zstsrc_spi_data {
	struct zstnode_common_data common;
#if defined(CONFIG_SPI_ASYNC)
	uint8_t dma_rx_buf[CONFIG_ZSTNODE_SPI_DMA_RX_BUF_SIZE];
	struct k_poll_signal sig;
	struct k_poll_event evt;
	bool async_enabled;
#endif
};

#if defined(CONFIG_SPI_ASYNC)

static int zstsrc_spi_run_async(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	struct zstsrc_spi_data *data = dev->data;
	struct net_buf *buf;
	size_t rx_len;
	int ret, result;

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
		return 0;
	}

	ret = k_poll(&data->evt, 1, K_MSEC(1000));
	if (ret < 0) {
		LOG_WRN("SPI RX poll timeout");
		return 0;
	}

	result = data->sig.result;
	if (result < 0) {
		LOG_ERR("SPI RX error: %d", result);
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

static int zstsrc_spi_open_async(const struct device *dev)
{
	const struct zstsrc_spi_config *cfg = dev->config;
	struct zstsrc_spi_data *data = dev->data;
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

	ret = spi_read_dt(&cfg->spi, &rx_bufs);
	if (ret < 0) {
		LOG_ERR("SPI read failed: %d", ret);
		net_buf_unref(buf);
		k_msleep(1);
		return 0;
	}

	net_buf_add(buf, rx_len);

	ret = zstreamer_submit_buffer(dev, buf);

	/* On real hardware the SPI transaction itself takes time; on
	 * emulators it completes instantly.  Sleep briefly to avoid
	 * busy-looping and to let the simulated clock advance.
	 */
	k_msleep(1);

	return ret;
}

static int zstsrc_spi_run(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	struct zstsrc_spi_data *data = dev->data;

	if (data->async_enabled) {
		return zstsrc_spi_run_async(dev);
	}
#endif
	return zstsrc_spi_run_poll(dev);
}

static int zstsrc_spi_open(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	zstsrc_spi_open_async(dev);
#endif
	return 0;
}

static int zstsrc_spi_close(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	struct zstsrc_spi_data *data = dev->data;

	data->async_enabled = false;
#endif
	return 0;
}

static const struct zstnode_driver_api zstsrc_spi_api = {
	.open = zstsrc_spi_open,
	.close = zstsrc_spi_close,
	.run = zstsrc_spi_run,
};

#if defined(CONFIG_SPI_ASYNC)
static int zstsrc_spi_init(const struct device *dev)
{
	struct zstsrc_spi_data *data = dev->data;

	k_poll_signal_init(&data->sig);
	k_poll_event_init(&data->evt, K_POLL_TYPE_SIGNAL,
			  K_POLL_MODE_NOTIFY_ONLY, &data->sig);
	return 0;
}
#define ZSTSRC_SPI_INIT_FN zstsrc_spi_init
#else
#define ZSTSRC_SPI_INIT_FN NULL
#endif

#define SPI_DEV_NODE(inst) DT_INST_PHANDLE(inst, spi_device)

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
		.spi = SPI_DT_SPEC_GET(SPI_DEV_NODE(inst),                    \
				       SPI_OP_MODE_MASTER | SPI_WORD_SET(8),   \
				       0),                                     \
		.rx_length = DT_INST_PROP(inst, rx_length),                    \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, ZSTSRC_SPI_INIT_FN)                \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsrc_spi_data_##inst,                                       \
		&zstsrc_spi_config_##inst,                                     \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsrc_spi_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSRC_SPI_DEFINE)
