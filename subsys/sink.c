/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(zstreamer_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Thread entry                                                        */
/* ------------------------------------------------------------------ */

int zstreamer_sink_common_init(const struct device *dev) {
  return zstreamer_node_common_init(dev);
}

/* ------------------------------------------------------------------ */
/* Thread entry                                                        */
/* ------------------------------------------------------------------ */

void zstreamer_sink_thread_entry(void *p1, void *p2, void *p3) {
  const struct device *dev = (const struct device *)p1;
  struct zstreamer_sink_data *data = (struct zstreamer_sink_data *)dev->data;
  const struct zstreamer_node_driver_api *api =
      (const struct zstreamer_node_driver_api *)dev->api;

  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  while (true) {
    struct net_buf *buf = k_fifo_get(&data->common.fifo, K_FOREVER);

    if (buf == NULL) {
      continue;
    }

    if (zstreamer_buf_is_event(buf)) {
      /* Sinks have no children: the dispatch helper runs the driver's
       * handle_event hook and then just drops the buffer. */
      zstreamer_node_dispatch_event(dev, buf, NULL, 0);
      continue;
    }

    int ret = api->process(dev, buf);

    if (ret != 0) {
      LOG_ERR("[%s] process error: %d", dev->name, ret);
    }

    net_buf_unref(buf);
  }
}
