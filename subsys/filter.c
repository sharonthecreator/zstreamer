/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/filter.h>

LOG_MODULE_REGISTER(zstreamer_filter, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Thread entry                                                        */
/* ------------------------------------------------------------------ */

int zstreamer_filter_common_init(const struct device *dev) {
  return zstreamer_node_common_init(dev);
}

/* ------------------------------------------------------------------ */
/* Thread entry                                                        */
/* ------------------------------------------------------------------ */

void zstreamer_filter_thread_entry(void *p1, void *p2, void *p3) {
  const struct device *dev = (const struct device *)p1;
  struct zstreamer_filter_data *data =
      (struct zstreamer_filter_data *)dev->data;
  const struct zstreamer_filter_config *cfg =
      (const struct zstreamer_filter_config *)dev->config;
  const struct zstreamer_node_driver_api *api =
      (const struct zstreamer_node_driver_api *)dev->api;

  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  while (true) {
    struct net_buf *buf = k_fifo_get(&data->common.fifo, K_FOREVER);

    if (buf == NULL) {
      continue;
    }

    int result = api->process(dev, buf);

    if (result < 0) {
      LOG_ERR("[%s] filter error: %d", dev->name, result);
      net_buf_unref(buf);
    } else if (result > 0) {
      zstreamer_node_distribute(dev, buf, cfg->common.children,
                                cfg->common.num_children);
    } else {
      zstreamer_node_distribute(dev, buf, cfg->false_children,
                                cfg->num_false_children);
    }
  }
}
