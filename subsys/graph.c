/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/graph.h>

LOG_MODULE_REGISTER(zstreamer_graph, CONFIG_ZSTREAMER_LOG_LEVEL);

#define DT_DRV_COMPAT zstreamer_graph

#define GRAPH_POOL_DEFINE(inst)                                                \
  NET_BUF_POOL_FIXED_DEFINE(graph_pool_##inst,                                 \
                            DT_INST_PROP(inst, buffer_count),                  \
                            DT_INST_PROP(inst, buffer_size), 0, NULL)

#define GRAPH_DEVICE_DEFINE(inst)                                              \
  GRAPH_POOL_DEFINE(inst);                                                     \
  static const struct zstreamer_graph_config graph_config_##inst = {           \
      .pool = &graph_pool_##inst,                                              \
  };                                                                           \
  static struct zstreamer_graph_data graph_data_##inst;                        \
  DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &graph_data_##inst,                  \
                        &graph_config_##inst, POST_KERNEL,                     \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(GRAPH_DEVICE_DEFINE)
