/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Batch processor node for zstreamer.
 *
 * Holds incoming net_bufs until batch-count have accumulated, then
 * releases them all downstream in arrival order.  Turns a steady
 * stream into periodic bursts — e.g. SD-card writes every N buffers
 * so the card idles between bursts.
 *
 * Held buffers stay allocated from the shared graph pool: the graph's
 * buffer-count must exceed batch-count by the pipeline's working set,
 * or upstream allocation starves while a batch accumulates.
 *
 * In process(): ref + park the buffer in held_batch and return -EAGAIN
 * (the framework's unref on that return drops only its own ref).  On
 * the Nth buffer, distribute every parked buffer manually and still
 * return -EAGAIN — the current buffer went downstream via the fifo
 * like the rest.
 */

#define DT_DRV_COMPAT zstreamer_batch_node

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(batch_node, CONFIG_ZSTREAMER_LOG_LEVEL);

struct batch_node_config {
	struct zstreamer_node_config common;
	uint32_t batch_count;
};

struct batch_node_data {
	struct zstreamer_node_data common;
	struct k_fifo held_batch;
	uint32_t held_count;
};

static int batch_node_process(const struct device *dev, struct net_buf *buf)
{
	const struct batch_node_config *cfg = dev->config;
	struct batch_node_data *data = dev->data;

	/* Our ref keeps the buffer alive in held_batch after the framework
	 * unrefs its own on the -EAGAIN return. */
	struct net_buf *held_ref = net_buf_ref(buf);

	k_fifo_put(&data->held_batch, held_ref);
	data->held_count++;

	if (data->held_count < cfg->batch_count) {
		return -EAGAIN;
	}

	struct net_buf *held;

	while ((held = k_fifo_get(&data->held_batch, K_NO_WAIT)) != NULL) {
		zstreamer_node_distribute(dev, held, cfg->common.children,
					  cfg->common.num_children);
	}
	data->held_count = 0;

	return -EAGAIN;
}

static int batch_node_init(const struct device *dev)
{
	const struct batch_node_config *cfg = dev->config;
	struct batch_node_data *data = dev->data;

	k_fifo_init(&data->held_batch);

	LOG_INF("batch_node %s: batch=%u", dev->name, cfg->batch_count);

	return zstreamer_node_common_init(dev);
}

static const struct zstreamer_node_driver_api batch_node_api = {
	.process = batch_node_process,
};

#define BATCH_NODE_DEFINE(inst)                                                                    \
	BUILD_ASSERT(DT_INST_PROP(inst, batch_count) > 0, "batch-count must be > 0");              \
	BUILD_ASSERT(DT_INST_PROP(inst, batch_count) <                                             \
			     DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_count),                  \
		     "batch-count must be below the graph's buffer-count or the "                  \
		     "pool starves while a batch accumulates");                                    \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct batch_node_data batch_node_data_##inst = {                                   \
		.common = ZSTREAMER_NODE_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct batch_node_config batch_node_config_##inst = {                         \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.batch_count = DT_INST_PROP(inst, batch_count),                                    \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, batch_node_init, NULL, &batch_node_data_##inst,                \
			      &batch_node_config_##inst, POST_KERNEL,                              \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &batch_node_api);

DT_INST_FOREACH_STATUS_OKAY(BATCH_NODE_DEFINE)
