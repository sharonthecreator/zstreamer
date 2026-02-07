/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstnode.h>
#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(zstreamer_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static struct net_buf *alloc_buf(const struct device *dev,
				 k_timeout_t timeout)
{
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;
	const struct zstreamer_graph_config *graph_cfg =
		(const struct zstreamer_graph_config *)cfg->graph->config;

	return net_buf_alloc_fixed(graph_cfg->pool, timeout);
}

static void distribute_to_children(const struct device *dev,
				    struct net_buf *buf)
{
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;

	if (cfg->num_children == 0) {
		net_buf_unref(buf);
		return;
	}

	/* Clone for all children except the last. */
	for (size_t i = 0; i < cfg->num_children - 1; i++) {
		const struct device *child = cfg->children[i];
		struct zstnode_common_data *child_data =
			(struct zstnode_common_data *)child->data;
		struct net_buf *clone = net_buf_clone(buf, K_NO_WAIT);

		if (clone != NULL) {
			k_fifo_put(&child_data->fifo, clone);
		} else {
			LOG_ERR("clone failed for %s", child->name);
		}
	}

	/* Last child gets the original buffer (transfer ownership). */
	{
		const struct device *child =
			cfg->children[cfg->num_children - 1];
		struct zstnode_common_data *child_data =
			(struct zstnode_common_data *)child->data;

		k_fifo_put(&child_data->fifo, buf);
	}
}

static void drain_fifo(struct k_fifo *fifo)
{
	struct net_buf *buf;

	while ((buf = k_fifo_get(fifo, K_NO_WAIT)) != NULL) {
		net_buf_unref(buf);
	}
}

/* ------------------------------------------------------------------ */
/* Thread entry point                                                  */
/* ------------------------------------------------------------------ */

static void process_thread_entry(void *p1, void *p2, void *p3)
{
	const struct device *dev = (const struct device *)p1;
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (api->generate != NULL) {
		/* Source: wait for start signal, then alloc+process+distribute. */
		while (true) {
			k_sem_take(&data->run_sem, K_FOREVER);

			while (atomic_get(&data->running)) {
				struct net_buf *buf =
					alloc_buf(dev, K_MSEC(100));

				if (buf == NULL) {
					continue;
				}

				int ret = api->generate(dev, buf);

				if (ret != 0) {
					net_buf_unref(buf);
					continue;
				}

				distribute_to_children(dev, buf);
				k_yield();
			}

			k_sem_give(&data->idle_sem);
		}
	} else {
		/* Non-source: dequeue from fifo, process, distribute. */
		while (true) {
			struct net_buf *buf =
				k_fifo_get(&data->fifo, K_FOREVER);

			if (buf == NULL) {
				continue;
			}

			int ret = api->process(dev, buf);

			if (ret != 0) {
				LOG_ERR("%s process error: %d",
					dev->name, ret);
				net_buf_unref(buf);
				continue;
			}

			distribute_to_children(dev, buf);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Common init                                                         */
/* ------------------------------------------------------------------ */

int zstnode_common_init(const struct device *dev)
{
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

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
				LOG_ERR("%s open failed: %d", dev->name, ret);
				return ret;
			}
		}
	}

	/* All nodes get their thread at boot. */
	k_thread_create(&data->thread, data->stack,
			cfg->thread_stack_size,
			process_thread_entry,
			(void *)dev, NULL, NULL,
			cfg->thread_priority, 0, K_NO_WAIT);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int zstnode_start(const struct device *dev)
{
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

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

int zstnode_stop(const struct device *dev)
{
	struct zstnode_common_data *data =
		(struct zstnode_common_data *)dev->data;
	const struct zstnode_driver_api *api =
		(const struct zstnode_driver_api *)dev->api;

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

struct net_buf *zstnode_alloc_buf(const struct device *dev,
				  k_timeout_t timeout)
{
	return alloc_buf(dev, timeout);
}
