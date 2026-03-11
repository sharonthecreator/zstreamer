/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_sine_src

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(sine_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct sine_src_config {
	struct zstreamer_source_config common;
	uint32_t cycle_ms;
};

struct sine_src_data {
	struct zstreamer_source_data common;
};

static int sine_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct sine_src_config *cfg = dev->config;

	while (net_buf_tailroom(buf) > 0) {
		int64_t now_ms = k_uptime_get();
		double angle = 2.0 * M_PI * (double)now_ms / (double)cfg->cycle_ms;
		double val = 128.0 + 127.0 * sin(angle);
		uint8_t sample = (uint8_t)(val + 0.5);

		net_buf_add_u8(buf, sample);
	}

	/* Allow simulated time to advance on native_sim / POSIX targets. */
	k_sleep(K_MSEC(1));

	return 0;
}

static const struct zstreamer_node_driver_api sine_src_api = {
	.process = sine_src_process,
};

#define SINE_SRC_DEFINE(inst)                                                                      \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	static struct sine_src_data sine_src_data_##inst = {                                       \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst)};                                       \
	static const struct sine_src_config sine_src_config_##inst = {                             \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.cycle_ms = DT_INST_PROP(inst, cycle_ms),                                          \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_source_common_init, NULL, &sine_src_data_##inst,     \
			      &sine_src_config_##inst, POST_KERNEL,                                \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &sine_src_api);

DT_INST_FOREACH_STATUS_OKAY(SINE_SRC_DEFINE)
