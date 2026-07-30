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
/* Shared helpers                                                     */
/* ------------------------------------------------------------------ */

void zstreamer_node_distribute(const struct device *dev, struct net_buf *buf,
                               const struct device *const *children,
                               size_t num_children) {
  struct zstreamer_node_data *child_data = NULL;
  const struct zstreamer_node_config *child_cfg = NULL;

  for (size_t i = 0; i < num_children; i++) {
    child_data = (struct zstreamer_node_data *)children[i]->data;
    child_cfg = (const struct zstreamer_node_config *)children[i]->config;

    /* Pass the buffer directly if the child node is readonly, if it's
     * an event (events are zero-length and immutable, never cloned),
     * or if it's the last child and the buffer has only one reference. */
    if (child_cfg->readonly || zstreamer_buf_is_event(buf) ||
        (buf->ref == 1 && i == (num_children - 1))) {
      buf = net_buf_ref(buf);
      k_fifo_put(&child_data->fifo, buf);
    }
    /* Clone the buffer for a readwrite child, unless it's the last one and
     * the buffer hasn't been shared. */
    else {
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
/* In-band stream events                                              */
/* ------------------------------------------------------------------ */

int zstreamer_node_send_event(const struct device *dev,
                              enum zstreamer_buf_type type,
                              k_timeout_t timeout) {
  const struct zstreamer_node_config *cfg =
      (const struct zstreamer_node_config *)dev->config;
  struct net_buf *buf = zstreamer_node_alloc_buf(dev, timeout);

  if (buf == NULL) {
    LOG_WRN("[%s] event %d alloc failed", dev->name, type);
    return -ENOMEM;
  }

  zstreamer_buf_type_set(buf, type);
  zstreamer_node_distribute(dev, buf, cfg->children, cfg->num_children);

  return 0;
}

void zstreamer_node_dispatch_event(const struct device *dev,
                                   struct net_buf *buf,
                                   const struct device *const *children,
                                   size_t num_children) {
  const struct zstreamer_node_driver_api *api =
      (const struct zstreamer_node_driver_api *)dev->api;
  int ret = (api->handle_event != NULL) ? api->handle_event(dev, buf) : 0;

  if (ret != 0) {
    /* Driver forwarded or consumed the event itself (-EAGAIN). */
    if (ret != -EAGAIN) {
      LOG_ERR("[%s] handle_event error: %d", dev->name, ret);
    }
    net_buf_unref(buf);
    return;
  }

  zstreamer_node_distribute(dev, buf, children, num_children);
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

    if (zstreamer_buf_is_event(buf)) {
      zstreamer_node_dispatch_event(dev, buf, cfg->children,
                                    cfg->num_children);
      continue;
    }

    int ret = api->process(dev, buf);

    if (ret != 0) {
      /* Silent unrefing if the node decides to drop the buffer. */
      if (ret != -EAGAIN) {
        LOG_ERR("[%s] process error: %d", dev->name, ret);
      }
      net_buf_unref(buf);
      continue;
    }

    zstreamer_node_distribute(dev, buf, cfg->children, cfg->num_children);
  }
}

/* ------------------------------------------------------------------ */
/* Common init                                                         */
/* ------------------------------------------------------------------ */

int zstreamer_node_common_init_stack(const struct device *dev,
                                     k_thread_stack_t *stack,
                                     size_t stack_size) {
  struct zstreamer_node_data *data = (struct zstreamer_node_data *)dev->data;
  const struct zstreamer_node_config *cfg =
      (const struct zstreamer_node_config *)dev->config;

  data->dev = dev;
  k_fifo_init(&data->fifo);

  k_thread_create(&data->thread, stack, stack_size, cfg->thread_entry,
                  (void *)dev, NULL, NULL, cfg->thread_priority, 0, K_NO_WAIT);

  return 0;
}

int zstreamer_node_common_init(const struct device *dev) {
  struct zstreamer_node_data *data = (struct zstreamer_node_data *)dev->data;

  return zstreamer_node_common_init_stack(dev, data->stack,
                                          ZSTREAMER_THREAD_STACK_SIZE);
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
