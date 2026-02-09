/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>
#include <zstreamer/graph.h>

LOG_MODULE_REGISTER(zstreamer_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static void distribute_to_children(const struct device *dev,
				    struct net_buf *buf)
{
	const struct zstreamer_node_config *cfg =
		(const struct zstreamer_node_config *)dev->config;

	/* Sharing the buffer with a single readwrite child node
	 * is only possible when we have ownership on our buffer
	 * in the first place. */
	bool shareable_buffer = !cfg->readonly;
	if (!cfg->readonly) {
		/* If a child who is readonly exists then we have no profit
		 * available trying to optimize the cloning, must pass the current
		 * buffer by reference to it. */
		for (size_t i = 0; i < cfg->num_children; i++) {
			const struct zstreamer_node_config *child_cfg =
				(const struct zstreamer_node_config *)
				cfg->children[i]->config;

			if (child_cfg->readonly) {
				shareable_buffer = false;
				break;
			}
		}
	}

	for (size_t i = 0; i < cfg->num_children; i++) {
		struct zstreamer_node_data *child_data =
			(struct zstreamer_node_data *)cfg->children[i]->data;
		const struct zstreamer_node_config *child_cfg =
			(const struct zstreamer_node_config *)
			cfg->children[i]->config;

		if (child_cfg->readonly || shareable_buffer) {
			net_buf_ref(buf);
			k_fifo_put(&child_data->fifo, buf);
			/* In case we entered because of a shared buffer. */
			shareable_buffer = false;
		}
		else {
			/* Readwrite child, clone the buf and send a fresh copy. */
			struct net_buf *clone =
				net_buf_clone(buf, K_NO_WAIT);

			if (clone != NULL) {
				k_fifo_put(&child_data->fifo, clone);
			} else {
				LOG_ERR("[%s] clone failed for %s",
					dev->name,
					cfg->children[i]->name);
			}
		}
	}

	net_buf_unref(buf);
}

static void drain_fifo(struct k_fifo *fifo)
{
	struct net_buf *buf;

	while ((buf = k_fifo_get(fifo, K_NO_WAIT)) != NULL) {
		net_buf_unref(buf);
	}
}

/* ------------------------------------------------------------------ */
/* Thread entry points                                                 */
/* ------------------------------------------------------------------ */

static void source_thread_entry(void *p1, void *p2, void *p3)
{
	const struct device *dev = (const struct device *)p1;
	struct zstreamer_node_data *data =
		(struct zstreamer_node_data *)dev->data;
	const struct zstreamer_node_driver_api *api =
		(const struct zstreamer_node_driver_api *)dev->api;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->run_sem, K_FOREVER);

		while (atomic_get(&data->running)) {
			struct net_buf *buf =
				zstreamer_node_alloc_buf(dev, K_MSEC(100));

			if (buf == NULL) {
				continue;
			}

			int ret = api->generate(dev, buf);

			if (ret != 0) {
				net_buf_unref(buf);
				LOG_ERR("[%s] generate failed: %d", dev->name, ret);
				continue;
			}

			distribute_to_children(dev, buf);
			k_yield();
		}

		k_sem_give(&data->idle_sem);
	}
}

static void process_thread_entry(void *p1, void *p2, void *p3)
{
	const struct device *dev = (const struct device *)p1;
	struct zstreamer_node_data *data =
		(struct zstreamer_node_data *)dev->data;
	const struct zstreamer_node_driver_api *api =
		(const struct zstreamer_node_driver_api *)dev->api;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		struct net_buf *buf =
			k_fifo_get(&data->fifo, K_FOREVER);

		if (buf == NULL) {
			continue;
		}

		int ret = api->process(dev, buf);

		if (ret != 0) {
			LOG_ERR("[%s] process error: %d",
				dev->name, ret);
			net_buf_unref(buf);
			continue;
		}

		distribute_to_children(dev, buf);
	}
}

/* ------------------------------------------------------------------ */
/* Common init                                                         */
/* ------------------------------------------------------------------ */

int zstreamer_node_common_init(const struct device *dev)
{
	struct zstreamer_node_data *data =
		(struct zstreamer_node_data *)dev->data;
	const struct zstreamer_node_config *cfg =
		(const struct zstreamer_node_config *)dev->config;
	const struct zstreamer_node_driver_api *api =
		(const struct zstreamer_node_driver_api *)dev->api;

	data->dev = dev;
	k_fifo_init(&data->fifo);

	if (api->generate != NULL) {
		k_sem_init(&data->run_sem, 0, 1);
		k_sem_init(&data->idle_sem, 0, 1);
	} else {
		/* Non-source: open hardware at boot. */
		if (api->open != NULL) {
			int ret = api->open(dev);

			if (ret != 0) {
				LOG_ERR("[%s] open failed: %d", dev->name, ret);
				return ret;
			}
		}
	}

	/* All nodes get their thread at boot. */
	k_thread_entry_t entry = (api->generate != NULL)
		? source_thread_entry : process_thread_entry;

	k_thread_create(&data->thread, data->stack,
			cfg->thread_stack_size,
			entry,
			(void *)dev, NULL, NULL,
			cfg->thread_priority, 0, K_NO_WAIT);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int zstreamer_node_start(const struct device *dev)
{
	struct zstreamer_node_data *data =
		(struct zstreamer_node_data *)dev->data;
	const struct zstreamer_node_driver_api *api =
		(const struct zstreamer_node_driver_api *)dev->api;

	if (api->generate == NULL) {
		return -ENOTSUP;
	}

	if (atomic_set(&data->running, 1) == 1) {
		return -EALREADY;
	}

	if (api->open != NULL) {
		int ret = api->open(dev);

		if (ret != 0) {
			atomic_set(&data->running, 0);
			return ret;
		}
	}

	k_sem_give(&data->run_sem);

	return 0;
}

int zstreamer_node_stop(const struct device *dev)
{
	struct zstreamer_node_data *data =
		(struct zstreamer_node_data *)dev->data;
	const struct zstreamer_node_driver_api *api =
		(const struct zstreamer_node_driver_api *)dev->api;

	if (api->generate == NULL) {
		return -ENOTSUP;
	}

	if (atomic_set(&data->running, 0) == 0) {
		return -EALREADY;
	}

	/* Wait for the source thread to become idle. */
	k_sem_take(&data->idle_sem, K_FOREVER);

	drain_fifo(&data->fifo);

	if (api->close != NULL) {
		api->close(dev);
	}

	return 0;
}

struct net_buf *zstreamer_node_alloc_buf(const struct device *dev,
				  k_timeout_t timeout)
{
	const struct zstreamer_node_config *cfg =
		(const struct zstreamer_node_config *)dev->config;
	const struct zstreamer_graph_config *graph_cfg =
		(const struct zstreamer_graph_config *)cfg->graph->config;

	return net_buf_alloc_fixed(graph_cfg->pool, timeout);
}
