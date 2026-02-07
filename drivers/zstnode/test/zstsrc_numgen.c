/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_zstsrc_numgen

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstnode.h>

LOG_MODULE_REGISTER(zstsrc_numgen, CONFIG_ZSTNODE_LOG_LEVEL);

struct zstsrc_numgen_config {
	struct zstnode_common_config common;
};

struct zstsrc_numgen_data {
	struct zstnode_common_data common;
	uint8_t counter;
};

static int zstsrc_numgen_process(const struct device *dev,
				 struct net_buf *buf)
{
	struct zstsrc_numgen_data *data = dev->data;

	while (net_buf_tailroom(buf) > 0) {
		net_buf_add_u8(buf, data->counter++);
	}

	return 0;
}

static const struct zstnode_driver_api zstsrc_numgen_api = {
	.generate = zstsrc_numgen_process,
};

#define ZSTSRC_NUMGEN_DEFINE(inst)                                             \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct zstsrc_numgen_data zstsrc_numgen_data_##inst = {          \
		.common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                     \
			zstnode_stack_##inst),                                  \
	};                                                                     \
	static const struct zstsrc_numgen_config zstsrc_numgen_config_##inst = {\
		.common = Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                   \
			DT_DRV_INST(inst),                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)),                  \
	};                                                                     \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, NULL)                              \
	DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
		&zstsrc_numgen_data_##inst,                                    \
		&zstsrc_numgen_config_##inst,                                  \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&zstsrc_numgen_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSRC_NUMGEN_DEFINE)
