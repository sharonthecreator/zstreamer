/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_numgen_src

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(src_numgen, CONFIG_ZSTREAMER_LOG_LEVEL);

struct src_numgen_config {
	struct zstreamer_node_config common;
};

struct src_numgen_data {
	struct zstreamer_node_data common;
	uint8_t counter;
};

static int src_numgen_process(const struct device *dev,
				 struct net_buf *buf)
{
	struct src_numgen_data *data = dev->data;

	while (net_buf_tailroom(buf) > 0) {
		net_buf_add_u8(buf, data->counter++);
	}

	return 0;
}

static const struct zstreamer_node_driver_api src_numgen_api = {
	.generate = src_numgen_process,
};

#define SRC_NUMGEN_DEFINE(inst)                                             \
	Z_ZSTREAMER_NODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
	static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                      \
		DT_INST_PROP(inst, thread_stack_size));                         \
	static struct src_numgen_data src_numgen_data_##inst = {          \
		.common = Z_ZSTREAMER_NODE_DATA_INIT(inst,                     \
			zstreamer_node_stack_##inst),                                  \
	};                                                                     \
	static const struct src_numgen_config src_numgen_config_##inst = {\
		.common = { Z_ZSTREAMER_NODE_CONFIG_INIT(inst,                 \
			DT_DRV_INST(inst),                                     \
			DT_INST_PROP(inst, thread_stack_size),                 \
			DT_INST_PROP(inst, thread_priority)) },                \
	};                                                                     \
	Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, NULL)                              \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_node_init_##inst, NULL,                 \
		&src_numgen_data_##inst,                                    \
		&src_numgen_config_##inst,                                  \
		POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
		&src_numgen_api);

DT_INST_FOREACH_STATUS_OKAY(SRC_NUMGEN_DEFINE)
