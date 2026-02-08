/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsink_fake

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstnode.h>

LOG_MODULE_REGISTER(zstsink_fake, CONFIG_ZSTNODE_LOG_LEVEL);

struct zstsink_fake_config {
	struct zstnode_common_config common;
};

struct zstsink_fake_data {
	struct zstnode_common_data common;
};

static int zstsink_fake_process(const struct device *dev,
				struct net_buf *buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);

	return 0;
}

static const struct zstnode_driver_api zstsink_fake_api = {
	.process = zstsink_fake_process,
};

#define ZSTSINK_FAKE_DEFINE(inst)                                              \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct zstsink_fake_data zstsink_fake_data_##inst = {           \
		.common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                     \
			zstnode_stack_##inst),                                  \
	};                                                                     \
	static const struct zstsink_fake_config zstsink_fake_config_##inst = { \
		.common = { Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                 \
			DT_DRV_INST(inst),                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)) },                \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, NULL)                              \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsink_fake_data_##inst,                                     \
		&zstsink_fake_config_##inst,                                   \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsink_fake_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSINK_FAKE_DEFINE)
