/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S source streaming node driver.
 *
 * Captures audio samples from an I2S microphone using Zephyr's I2S API.
 * Each call to process() performs a blocking i2s_read() to receive a
 * block of audio data and copies it into a net_buf for downstream
 * processing.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(i2s_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#define DT_DRV_COMPAT zstreamer_i2s_src

struct i2s_src_config {
	struct zstreamer_source_config common;
	const struct device *i2s_dev;
	struct k_mem_slab *rx_slab;
	uint32_t sample_rate_hz;
	uint8_t word_size;
	uint8_t num_channels;
	uint16_t block_size;
};

struct i2s_src_data {
	struct zstreamer_source_data common;
	bool stream_started;
};

static int i2s_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct i2s_src_config *cfg = dev->config;
	struct i2s_src_data *data = dev->data;
	void *rx_block;
	size_t rx_size;
	int ret;

	if (!data->stream_started) {
		ret = i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		if (ret < 0) {
			LOG_ERR("i2s_trigger START failed: %d", ret);
			return ret;
		}
		data->stream_started = true;
	}

	ret = i2s_read(cfg->i2s_dev, &rx_block, &rx_size);
	if (ret < 0) {
		LOG_ERR("i2s_read failed: %d", ret);
		if (ret == -EIO) {
			/* Attempt to recover from error state. */
			i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
			data->stream_started = false;
		}
		return ret;
	}

	size_t copy_len = MIN(rx_size, net_buf_tailroom(buf));
	net_buf_add_mem(buf, rx_block, copy_len);

	k_mem_slab_free(cfg->rx_slab, rx_block);

	return 0;
}

static int i2s_src_init(const struct device *dev)
{
	const struct i2s_src_config *cfg = dev->config;
	int ret;

	if (!device_is_ready(cfg->i2s_dev)) {
		LOG_ERR("I2S device %s not ready", cfg->i2s_dev->name);
		return -ENODEV;
	}

	struct i2s_config i2s_cfg = {
		.word_size = cfg->word_size,
		.channels = cfg->num_channels,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
		.frame_clk_freq = cfg->sample_rate_hz,
		.block_size = cfg->block_size,
		.mem_slab = cfg->rx_slab,
		.timeout = K_MSEC(1000),
	};

	ret = i2s_configure(cfg->i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure RX failed: %d", ret);
		return ret;
	}

	LOG_INF("I2S source %s: %u Hz, %u-bit, %u ch, block %u B", dev->name, cfg->sample_rate_hz,
		cfg->word_size, cfg->num_channels, cfg->block_size);

	return zstreamer_source_common_init(dev);
}

static const struct zstreamer_node_driver_api i2s_src_api = {
	.process = i2s_src_process,
};

#define I2S_SRC_DEFINE(inst)                                                                       \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
                                                                                                   \
	K_MEM_SLAB_DEFINE_STATIC(i2s_src_rx_slab_##inst, DT_INST_PROP(inst, block_size),           \
				 CONFIG_ZSTREAMER_I2S_SLAB_COUNT, 4);                              \
                                                                                                   \
	static struct i2s_src_data i2s_src_data_##inst = {                                         \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct i2s_src_config i2s_src_config_##inst = {                               \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.i2s_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, i2s_device)),                       \
		.rx_slab = &i2s_src_rx_slab_##inst,                                                \
		.sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                              \
		.word_size = DT_INST_PROP(inst, word_size),                                        \
		.num_channels = DT_INST_PROP_OR(inst, num_channels, 1),                            \
		.block_size = DT_INST_PROP(inst, block_size),                                      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, i2s_src_init, NULL, &i2s_src_data_##inst,                      \
			      &i2s_src_config_##inst, POST_KERNEL,                                 \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &i2s_src_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_SRC_DEFINE)
