/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(zstreamer_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Thread entry points                                                 */
/* ------------------------------------------------------------------ */

static void source_thread_entry(void *p1, void *p2, void *p3)
{
	const struct device *dev = (const struct device *)p1;
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (atomic_get(&data->running)) {
		int ret = api->run(dev);

		if (ret != 0) {
			LOG_ERR("source %s run returned %d, stopping",
				dev->name, ret);
			break;
		}
	}
}

static void sink_thread_entry(void *p1, void *p2, void *p3)
{
	const struct device *dev = (const struct device *)p1;
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (atomic_get(&data->running)) {
		struct net_buf *buf = k_fifo_get(&data->fifo, K_MSEC(100));

		if (buf == NULL) {
			continue;
		}

		int ret = api->process(dev, buf);

		net_buf_unref(buf);

		if (ret != 0) {
			LOG_ERR("sink %s process returned %d", dev->name, ret);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Generic work handler                                                */
/* ------------------------------------------------------------------ */

void zstnode_generic_work_handler(struct k_work *work)
{
	struct zstnode_common_data *data =
		CONTAINER_OF(work, struct zstnode_common_data, work);
	const struct device *dev = data->dev;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;
	struct net_buf *buf;

	while ((buf = k_fifo_get(&data->fifo, K_NO_WAIT)) != NULL) {
		int ret = api->process(dev, buf);

		net_buf_unref(buf);

		if (ret != 0) {
			LOG_ERR("generic %s process returned %d",
				dev->name, ret);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Fifo drain helper                                                   */
/* ------------------------------------------------------------------ */

static void drain_fifo(struct k_fifo *fifo)
{
	struct net_buf *buf;

	while ((buf = k_fifo_get(fifo, K_NO_WAIT)) != NULL) {
		net_buf_unref(buf);
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int zstreamer_start(const struct device *dev)
{
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;
	int ret = 0;

	if (atomic_set(&data->running, 1) == 1) {
		return -EALREADY;
	}

	if (api->open != NULL) {
		ret = api->open(dev);
		if (ret != 0) {
			atomic_set(&data->running, 0);
			return ret;
		}
	}

	if (cfg->type == ZSTNODE_TYPE_SOURCE) {
		k_thread_create(&data->thread, data->stack,
				cfg->thread_stack_size,
				source_thread_entry,
				(void *)dev, NULL, NULL,
				cfg->thread_priority, 0, K_NO_WAIT);
	} else if (cfg->type == ZSTNODE_TYPE_SINK) {
		k_thread_create(&data->thread, data->stack,
				cfg->thread_stack_size,
				sink_thread_entry,
				(void *)dev, NULL, NULL,
				cfg->thread_priority, 0, K_NO_WAIT);
	}
	/* Generic nodes don't need a thread — work is submitted on demand. */

	return 0;
}

int zstreamer_stop(const struct device *dev)
{
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

	if (atomic_set(&data->running, 0) == 0) {
		return -EALREADY;
	}

	if (cfg->type == ZSTNODE_TYPE_SOURCE ||
	    cfg->type == ZSTNODE_TYPE_SINK) {
		k_thread_join(&data->thread, K_FOREVER);
	}

	drain_fifo(&data->fifo);

	if (api->close != NULL) {
		api->close(dev);
	}

	return 0;
}

int zstreamer_submit_buffer(const struct device *dev, struct net_buf *buf)
{
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;

	if (cfg->num_children == 0) {
		net_buf_unref(buf);
		return 0;
	}

	for (size_t i = 0; i < cfg->num_children; i++) {
		const struct device *child = cfg->children[i];
		struct zstnode_common_data *child_data =
			(struct zstnode_common_data *)child->data;
		const struct zstnode_common_config *child_cfg =
			(const struct zstnode_common_config *)child->config;
		struct net_buf *child_buf;

		if (i == 0) {
			child_buf = net_buf_ref(buf);
		} else {
			child_buf = net_buf_clone(buf, K_NO_WAIT);
			if (child_buf == NULL) {
				LOG_ERR("failed to clone buf for child %s",
					child->name);
				continue;
			}
		}

		k_fifo_put(&child_data->fifo, child_buf);

		if (child_cfg->type == ZSTNODE_TYPE_GENERIC) {
			k_work_submit(&child_data->work);
		}
	}

	/* Release the caller's reference. */
	net_buf_unref(buf);

	return 0;
}

struct net_buf *zstreamer_alloc_buf(const struct device *dev,
				    k_timeout_t timeout)
{
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;
	const struct zstreamer_graph_config *graph_cfg =
		(const struct zstreamer_graph_config *)cfg->graph->config;

	return net_buf_alloc_fixed(graph_cfg->pool, timeout);
}
