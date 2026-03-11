/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_passthrough_node

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(passthrough_node, CONFIG_ZSTREAMER_LOG_LEVEL);

static int passthrough_node_process(const struct device *dev, struct net_buf *buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);

	return 0;
}

static const struct zstreamer_node_driver_api passthrough_node_api = {
	.process = passthrough_node_process,
};

#define PASSTHROUGH_NODE_DEFINE(inst)                                                              \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct zstreamer_node_data passthrough_node_data_##inst =                           \
		ZSTREAMER_NODE_DATA_INIT(inst);                                                    \
	static const struct zstreamer_node_config passthrough_node_config_##inst =                 \
		ZSTREAMER_NODE_CONFIG_INIT(inst, true);                                            \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,                              \
			      &passthrough_node_data_##inst, &passthrough_node_config_##inst,      \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                     \
			      &passthrough_node_api);

DT_INST_FOREACH_STATUS_OKAY(PASSTHROUGH_NODE_DEFINE)
