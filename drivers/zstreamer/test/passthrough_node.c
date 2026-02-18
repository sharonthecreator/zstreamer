/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_passthrough_node

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(passthrough_node, CONFIG_ZSTREAMER_LOG_LEVEL);

struct passthrough_node_config {
  struct zstreamer_node_config common;
};

struct passthrough_node_data {
  struct zstreamer_node_data common;
};

static int passthrough_node_process(const struct device *dev,
                                    struct net_buf *buf) {
  ARG_UNUSED(dev);
  ARG_UNUSED(buf);

  return 0;
}

static const struct zstreamer_node_driver_api passthrough_node_api = {
    .process = passthrough_node_process,
};

#define PASSTHROUGH_NODE_DEFINE(inst)                                          \
  ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                     \
  static struct passthrough_node_data passthrough_node_data_##inst = {         \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(                                    \
          zstreamer_node_stack_##inst),                                        \
  };                                                                           \
  static const struct passthrough_node_config                                  \
      passthrough_node_config_##inst = {                                       \
          .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(                             \
              DT_DRV_INST(inst),                                               \
              DT_INST_PROP(inst, thread_stack_size),                            \
              DT_INST_PROP(inst, thread_priority),                             \
              zstreamer_node_children_##inst,                                  \
              Z_ZSTREAMER_NUM_CHILDREN(DT_DRV_INST(inst)))},                   \
  };                                                                           \
  ZSTREAMER_NODE_DT_INST_DEFINE(inst, NULL,                                    \
                                &passthrough_node_data_##inst,                 \
                                &passthrough_node_config_##inst,               \
                                &passthrough_node_api);

DT_INST_FOREACH_STATUS_OKAY(PASSTHROUGH_NODE_DEFINE)
