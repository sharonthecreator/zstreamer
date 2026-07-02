/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Looper processor node for zstreamer.
 *
 * Retransmits each incoming net_buf a configurable number of times.
 * Useful for reliability over lossy transports (e.g. LoRa) — repeat
 * the same packet multiple times without allocating extra memory.
 *
 * In process(): call zstreamer_node_distribute() manually
 * (retransmit_count - 1) times with net_buf_ref() to keep the buf
 * alive, then return 0 so the framework does the final distribute.
 */

#define DT_DRV_COMPAT zstreamer_looper_node

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(looper_node, CONFIG_ZSTREAMER_LOG_LEVEL);

struct looper_node_config {
	struct zstreamer_node_config common;
	uint32_t retransmit_count;
	uint32_t delay_ms;
};

struct looper_node_data {
	struct zstreamer_node_data common;
};

static int looper_node_process(const struct device *dev, struct net_buf *buf)
{
	const struct looper_node_config *cfg = dev->config;

	/* Send (retransmit_count - 1) extra copies manually. */
	for (uint32_t i = 1; i < cfg->retransmit_count; i++) {
		struct net_buf *ref = net_buf_ref(buf);
		ARG_UNUSED(ref);
		zstreamer_node_distribute(dev, buf, cfg->common.children, cfg->common.num_children);

		if (cfg->delay_ms > 0) {
			k_msleep(cfg->delay_ms);
		}
	}

	/* Return 0 — framework does the final (Nth) distribute and unrefs. */
	return 0;
}

static int looper_node_init(const struct device *dev)
{
	const struct looper_node_config *cfg = dev->config;

	LOG_INF("looper_node %s: retransmit=%u delay=%u ms", dev->name, cfg->retransmit_count,
		cfg->delay_ms);

	return zstreamer_node_common_init(dev);
}

static const struct zstreamer_node_driver_api looper_node_api = {
	.process = looper_node_process,
};

#define LOOPER_NODE_DEFINE(inst)                                                                   \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct looper_node_data looper_node_data_##inst = {                                 \
		.common = ZSTREAMER_NODE_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct looper_node_config looper_node_config_##inst = {                       \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.retransmit_count = DT_INST_PROP(inst, retransmit_count),                          \
		.delay_ms = DT_INST_PROP(inst, delay_ms),                                          \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, looper_node_init, NULL, &looper_node_data_##inst,              \
			      &looper_node_config_##inst, POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &looper_node_api);

DT_INST_FOREACH_STATUS_OKAY(LOOPER_NODE_DEFINE)
