/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/processor.h>

LOG_MODULE_DECLARE(zstreamer_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Thread entry                                                        */
/* ------------------------------------------------------------------ */

static void processor_thread_entry(void *p1, void *p2, void *p3) {
  const struct device *dev = (const struct device *)p1;
  struct zstreamer_processor_data *data =
      (struct zstreamer_processor_data *)dev->data;
  const struct zstreamer_processor_config *cfg =
      (const struct zstreamer_processor_config *)dev->config;
  const struct zstreamer_processor_driver_api *api =
      (const struct zstreamer_processor_driver_api *)dev->api;

  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  while (true) {
    struct net_buf *buf = k_fifo_get(&data->common.fifo, K_FOREVER);

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

int zstreamer_processor_common_init(const struct device *dev) {
  const struct zstreamer_processor_driver_api *api =
      (const struct zstreamer_processor_driver_api *)dev->api;

  if (api->open != NULL) {
    int ret = api->open(dev);

    if (ret != 0) {
      LOG_ERR("[%s] open failed: %d", dev->name, ret);
      return ret;
    }
  }

  return zstreamer_node_base_init(dev, processor_thread_entry);
}
