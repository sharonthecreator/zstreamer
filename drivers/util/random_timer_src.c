/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Random timer source for zstreamer.
 *
 * Generates an empty buffer at random intervals between
 * min-range-seconds and max-range-seconds.
 */

#define DT_DRV_COMPAT zstreamer_random_timer_src

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/random/random.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(random_timer_src, CONFIG_ZSTREAMER_LOG_LEVEL);

struct random_timer_src_config {
	struct zstreamer_source_config common;
	uint32_t min_range_seconds;
	uint32_t max_range_seconds;
};

struct random_timer_src_data {
	struct zstreamer_source_data common;
};

static int random_timer_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct random_timer_src_config *cfg = dev->config;
	uint32_t min = cfg->min_range_seconds;
	uint32_t max = cfg->max_range_seconds;

	uint32_t delay = sys_rand32_get() % (max - min + 1) + min;

	k_sleep(K_SECONDS(delay));

	return 0;
}

static const struct zstreamer_node_driver_api random_timer_src_api = {
	.process = random_timer_src_process,
};

#define RANDOM_TIMER_SRC_DEFINE(inst)                                                              \
	BUILD_ASSERT(DT_INST_PROP(inst, min_range_seconds) <=                                      \
			     DT_INST_PROP(inst, max_range_seconds),                                \
		     "min-range-seconds must not exceed max-range-seconds");                       \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	static struct random_timer_src_data random_timer_src_data_##inst = {                       \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct random_timer_src_config random_timer_src_config_##inst = {             \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.min_range_seconds = DT_INST_PROP(inst, min_range_seconds),                        \
		.max_range_seconds = DT_INST_PROP(inst, max_range_seconds),                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_source_common_init, NULL,                            \
			      &random_timer_src_data_##inst, &random_timer_src_config_##inst,      \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                     \
			      &random_timer_src_api);

DT_INST_FOREACH_STATUS_OKAY(RANDOM_TIMER_SRC_DEFINE)
