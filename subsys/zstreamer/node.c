/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/graph.h>
#include <zstreamer/node.h>

LOG_MODULE_REGISTER(zstreamer_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

void zstreamer_node_distribute(const struct device *dev, struct net_buf *buf,
                               const struct device *const *children,
                               size_t num_children) {
  const struct zstreamer_node_config *cfg =
      (const struct zstreamer_node_config *)dev->config;
  struct zstreamer_node_data *child_data = NULL;
  const struct zstreamer_node_config *child_cfg = NULL;

  if (num_children == 0) {
    net_buf_unref(buf);
    return;
  }

  /* Single stream optimization. */
  if (buf->ref == 1 && num_children == 1) {
    child_data = (struct zstreamer_node_data *)children[0]->data;
    k_fifo_put(&child_data->fifo, buf);
    return;
  }

  bool shareable_buffer = !cfg->readonly;
  if (!cfg->readonly) {
    for (size_t i = 0; i < num_children; i++) {
      child_cfg = (const struct zstreamer_node_config *)children[i]->config;

      if (child_cfg->readonly) {
        shareable_buffer = false;
        break;
      }
    }
  }

  for (size_t i = 0; i < num_children; i++) {
    child_data = (struct zstreamer_node_data *)children[i]->data;
    child_cfg = (const struct zstreamer_node_config *)children[i]->config;

    if (child_cfg->readonly || shareable_buffer) {
      buf = net_buf_ref(buf);
      k_fifo_put(&child_data->fifo, buf);
      shareable_buffer = false;
    } else {
      struct net_buf *clone = net_buf_clone(buf, K_NO_WAIT);

      if (clone != NULL) {
        k_fifo_put(&child_data->fifo, clone);
      } else {
        LOG_ERR("[%s] clone failed for %s", dev->name, children[i]->name);
      }
    }
  }

  net_buf_unref(buf);
}

void zstreamer_node_drain_fifo(struct k_fifo *fifo) {
  struct net_buf *buf;

  while ((buf = k_fifo_get(fifo, K_NO_WAIT)) != NULL) {
    net_buf_unref(buf);
  }
}

/* ------------------------------------------------------------------ */
/* Standard node thread entry                                         */
/* ------------------------------------------------------------------ */

void zstreamer_node_thread_entry(void *p1, void *p2, void *p3) {
  const struct device *dev = (const struct device *)p1;
  struct zstreamer_node_data *data = (struct zstreamer_node_data *)dev->data;
  const struct zstreamer_node_config *cfg =
      (const struct zstreamer_node_config *)dev->config;
  const struct zstreamer_node_driver_api *api =
      (const struct zstreamer_node_driver_api *)dev->api;

  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  while (true) {
    struct net_buf *buf = k_fifo_get(&data->fifo, K_FOREVER);

    if (buf == NULL) {
      continue;
    }

    int ret = api->process(dev, buf);

    if (ret != 0) {
      LOG_ERR("[%s] process error: %d", dev->name, ret);
      net_buf_unref(buf);
      continue;
    }

    zstreamer_node_distribute(dev, buf, cfg->children, cfg->num_children);
  }
}

/* ------------------------------------------------------------------ */
/* Common init                                                         */
/* ------------------------------------------------------------------ */

int zstreamer_node_common_init(const struct device *dev) {
  struct zstreamer_node_data *data = (struct zstreamer_node_data *)dev->data;
  const struct zstreamer_node_config *cfg =
      (const struct zstreamer_node_config *)dev->config;

  data->dev = dev;
  k_fifo_init(&data->fifo);

  k_thread_create(&data->thread, data->stack, cfg->thread_stack_size,
                  cfg->thread_entry, (void *)dev, NULL, NULL,
                  cfg->thread_priority, 0, K_NO_WAIT);

  return 0;
}

/* ------------------------------------------------------------------ */
/* Buffer allocation                                                   */
/* ------------------------------------------------------------------ */

struct net_buf *zstreamer_node_alloc_buf(const struct device *dev,
                                         k_timeout_t timeout) {
  const struct zstreamer_node_config *cfg =
      (const struct zstreamer_node_config *)dev->config;
  const struct zstreamer_graph_config *graph_cfg =
      (const struct zstreamer_graph_config *)cfg->graph->config;

  return net_buf_alloc_fixed(graph_cfg->pool, timeout);
}
