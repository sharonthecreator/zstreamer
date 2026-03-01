/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(zstreamer_source, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Thread entry                                                        */
/* ------------------------------------------------------------------ */

void zstreamer_source_thread_entry(void *p1, void *p2, void *p3) {
  const struct device *dev = (const struct device *)p1;
  struct zstreamer_source_data *data =
      (struct zstreamer_source_data *)dev->data;
  const struct zstreamer_source_config *cfg =
      (const struct zstreamer_source_config *)dev->config;
  const struct zstreamer_node_driver_api *api =
      (const struct zstreamer_node_driver_api *)dev->api;

  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  k_sem_init(&data->run_sem, 0, 1);
  k_sem_init(&data->idle_sem, 0, 1);

  while (true) {
    k_sem_take(&data->run_sem, K_FOREVER);

    while (atomic_get(&data->running)) {
      struct net_buf *buf = zstreamer_node_alloc_buf(dev, K_MSEC(100));

      if (buf == NULL) {
        continue;
      }

      int ret = api->process(dev, buf);

      if (ret != 0) {
        net_buf_unref(buf);
        LOG_ERR("[%s] generate failed: %d", dev->name, ret);
        continue;
      }

      zstreamer_node_distribute(dev, buf, cfg->common.children,
                                cfg->common.num_children);
      k_yield();
    }

    k_sem_give(&data->idle_sem);
  }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int zstreamer_source_start(const struct device *dev) {
  struct zstreamer_source_data *data =
      (struct zstreamer_source_data *)dev->data;

  if (atomic_set(&data->running, 1) == 1) {
    return -EALREADY;
  }

  k_sem_give(&data->run_sem);

  return 0;
}

int zstreamer_source_stop(const struct device *dev) {
  struct zstreamer_source_data *data =
      (struct zstreamer_source_data *)dev->data;

  if (atomic_set(&data->running, 0) == 0) {
    return -EALREADY;
  }

  if (k_sem_take(&data->idle_sem, K_SECONDS(5)) != 0) {
    LOG_WRN("[%s] stop timeout waiting for idle", dev->name);
  }

  zstreamer_node_drain_fifo(&data->common.fifo);

  return 0;
}
