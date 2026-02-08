/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsink_spi

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstnode.h>

LOG_MODULE_REGISTER(zstsink_spi, CONFIG_ZSTNODE_LOG_LEVEL);

struct zstsink_spi_config {
	struct zstnode_common_config common;
	struct spi_dt_spec spi;
};

struct zstsink_spi_data {
	struct zstnode_common_data common;
#if defined(CONFIG_SPI_ASYNC)
	struct k_poll_signal sig;
	struct k_poll_event evt;
	bool async_enabled;
#endif
};

#if defined(CONFIG_SPI_ASYNC)

static int zstsink_spi_process_async(const struct device *dev,
				     struct net_buf *buf)
{
	const struct zstsink_spi_config *cfg = dev->config;
	struct zstsink_spi_data *data = dev->data;
	int ret, result;

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

	k_poll_signal_reset(&data->sig);
	data->evt.state = K_POLL_STATE_NOT_READY;

	ret = spi_write_signal(cfg->spi.bus, &cfg->spi.config,
			       &tx_bufs, &data->sig);
	if (ret < 0) {
		LOG_ERR("SPI async write failed: %d", ret);
		return ret;
	}

	ret = k_poll(&data->evt, 1, K_FOREVER);
	if (ret < 0) {
		LOG_ERR("SPI TX poll failed: %d", ret);
		return ret;
	}

	result = data->sig.result;
	if (result < 0) {
		LOG_ERR("SPI TX error: %d", result);
	}

	return result;
}

static int zstsink_spi_open_async(const struct device *dev)
{
	const struct zstsink_spi_config *cfg = dev->config;
	struct zstsink_spi_data *data = dev->data;
	uint8_t dummy = 0;
	struct spi_buf test_buf = { .buf = &dummy, .len = 1 };
	struct spi_buf_set test_bufs = { .buffers = &test_buf, .count = 1 };
	int ret;

	k_poll_signal_reset(&data->sig);
	data->evt.state = K_POLL_STATE_NOT_READY;

	ret = spi_write_signal(cfg->spi.bus, &cfg->spi.config,
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
	LOG_DBG("SPI async TX enabled");
	return 0;
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

	return spi_write_dt(&cfg->spi, &tx_bufs);
}

static int zstsink_spi_process(const struct device *dev,
			       struct net_buf *buf)
{
#if defined(CONFIG_SPI_ASYNC)
	struct zstsink_spi_data *data = dev->data;

	if (data->async_enabled) {
		return zstsink_spi_process_async(dev, buf);
	}
#endif
	return zstsink_spi_process_poll(dev, buf);
}

static int zstsink_spi_open(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	zstsink_spi_open_async(dev);
#endif
	return 0;
}

static int zstsink_spi_close(const struct device *dev)
{
#if defined(CONFIG_SPI_ASYNC)
	struct zstsink_spi_data *data = dev->data;

	data->async_enabled = false;
#endif
	return 0;
}

static const struct zstnode_driver_api zstsink_spi_api = {
	.open = zstsink_spi_open,
	.close = zstsink_spi_close,
	.process = zstsink_spi_process,
};

#if defined(CONFIG_SPI_ASYNC)
static int zstsink_spi_init(const struct device *dev)
{
	struct zstsink_spi_data *data = dev->data;

	k_poll_signal_init(&data->sig);
	k_poll_event_init(&data->evt, K_POLL_TYPE_SIGNAL,
			  K_POLL_MODE_NOTIFY_ONLY, &data->sig);
	return 0;
}
#define ZSTSINK_SPI_INIT_FN zstsink_spi_init
#else
#define ZSTSINK_SPI_INIT_FN NULL
#endif

#define SPI_DEV_NODE(inst) DT_INST_PHANDLE(inst, spi_device)

#define ZSTSINK_SPI_DEFINE(inst)                                               \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct zstsink_spi_data zstsink_spi_data_##inst = {             \
		.common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                     \
			zstnode_stack_##inst),                                  \
	};                                                                     \
	static const struct zstsink_spi_config zstsink_spi_config_##inst = {   \
		.common = { Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                 \
			DT_DRV_INST(inst),                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)) },                \
		.spi = SPI_DT_SPEC_GET(SPI_DEV_NODE(inst),                    \
				       SPI_OP_MODE_MASTER | SPI_WORD_SET(8),   \
				       0),                                     \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, ZSTSINK_SPI_INIT_FN)               \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsink_spi_data_##inst,                                      \
		&zstsink_spi_config_##inst,                                    \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsink_spi_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSINK_SPI_DEFINE)
