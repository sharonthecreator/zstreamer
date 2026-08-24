/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S master-receive source for 24-bit-in-32 MEMS microphones
 * (e.g. INMP441).  Configures the referenced I2S/SAI peripheral as
 * master RX with 32-bit slots (mics like the INMP441 require the
 * 64 SCK/frame ratio; 32 SCK/frame makes them output duplicated
 * samples plus broadband noise) and emits one channel (`channel`
 * property) as int16 PCM.  process() loops i2s_read() until the
 * net_buf is full, so one emitted buffer spans graph buffer-size
 * bytes regardless of the DMA block size.
 */

#define DT_DRV_COMPAT zstreamer_i2s_src

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(i2s_src, CONFIG_ZSTREAMER_LOG_LEVEL);

/* Samples (per channel) per i2s_read block: small enough to keep the
 * DMA slab modest while process() accumulates blocks into the net_buf. */
#define I2S_SRC_BLOCK_SAMPLES 500U
/* The I2S frame always carries 2 slots; mono mics drive only the left. */
#define I2S_SRC_CHANNELS      2U
#define I2S_SRC_BLOCK_BYTES   (I2S_SRC_BLOCK_SAMPLES * I2S_SRC_CHANNELS * sizeof(int32_t))
#define I2S_SRC_BLOCK_COUNT   4U
/* 32-byte alignment for DMA burst transfers. */
#define I2S_SRC_DMA_ALIGN     32U

#define I2S_SRC_READ_TIMEOUT_MS 2000

struct i2s_src_config {
	struct zstreamer_source_config common;
	const struct device *i2s_dev;
	uint32_t sample_rate_hz;
	/* Frame slot to keep: 0 = left, 1 = right.  Must match the mic's
	 * channel-select strap -- the other slot is undriven. */
	uint8_t slot;
	struct k_mem_slab *slab;
};

struct i2s_src_data {
	struct zstreamer_source_data common;
	bool started;
};

static int i2s_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct i2s_src_config *cfg = dev->config;
	struct i2s_src_data *data = dev->data;
	int ret;

	if (!data->started) {
		ret = i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		if (ret < 0) {
			LOG_ERR("[%s] I2S START failed: %d", dev->name, ret);
			return ret;
		}
		data->started = true;
	}

	while (net_buf_tailroom(buf) >= I2S_SRC_BLOCK_SAMPLES * sizeof(int16_t)) {
		void *block = NULL;
		size_t size = 0;

		ret = i2s_read(cfg->i2s_dev, &block, &size);
		if (ret < 0) {
			/* RX overrun etc. put the peripheral in ERROR: re-arm and retry
			 * with a fresh buffer on the next process() call. */
			LOG_ERR("[%s] i2s_read error: %d - restarting stream", dev->name, ret);
			i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
			data->started = false;
			return -EAGAIN;
		}

		const int32_t *words = block;
		size_t samples = size / (I2S_SRC_CHANNELS * sizeof(int32_t));
		int16_t *dst = (int16_t *)net_buf_add(buf, samples * sizeof(int16_t));

		for (size_t i = 0; i < samples; i++) {
			/* Standard I2S puts the 24-bit word at slot bits [30:7] (bit 31
			 * is the 1-SCK delay bit), so >>15 keeps the top 16 signal bits. */
			dst[i] = (int16_t)(words[i * I2S_SRC_CHANNELS + cfg->slot] >> 15);
		}

		/* Free before looping so the DMA never starves for blocks. */
		k_mem_slab_free(cfg->slab, block);
	}

	return 0;
}

static const struct zstreamer_node_driver_api i2s_src_api = {
	.process = i2s_src_process,
};

static int i2s_src_init(const struct device *dev)
{
	const struct i2s_src_config *cfg = dev->config;

	if (!device_is_ready(cfg->i2s_dev)) {
		LOG_ERR("[%s] I2S device not ready", dev->name);
		return -ENODEV;
	}

	const struct i2s_config i2s_cfg = {
		.word_size = 32U,
		.channels = I2S_SRC_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
		.frame_clk_freq = cfg->sample_rate_hz,
		.mem_slab = cfg->slab,
		.block_size = I2S_SRC_BLOCK_BYTES,
		.timeout = I2S_SRC_READ_TIMEOUT_MS,
	};

	int ret = i2s_configure(cfg->i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("[%s] i2s_configure failed: %d", dev->name, ret);
		return ret;
	}

	return zstreamer_source_common_init(dev);
}

#define I2S_SRC_DEFINE(inst)                                                                       \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	K_MEM_SLAB_DEFINE_STATIC(i2s_src_slab_##inst, I2S_SRC_BLOCK_BYTES, I2S_SRC_BLOCK_COUNT,    \
				 I2S_SRC_DMA_ALIGN);                                               \
	static struct i2s_src_data i2s_src_data_##inst = {                                         \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct i2s_src_config i2s_src_config_##inst = {                               \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.i2s_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, i2s_device)),                       \
		.sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                              \
		.slot = DT_INST_ENUM_IDX(inst, channel),                                           \
		.slab = &i2s_src_slab_##inst,                                                      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, i2s_src_init, NULL, &i2s_src_data_##inst,                      \
			      &i2s_src_config_##inst, POST_KERNEL,                                 \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &i2s_src_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_SRC_DEFINE)
