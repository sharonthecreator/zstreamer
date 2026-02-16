/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_fake_sink

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(sink_fake, CONFIG_ZSTREAMER_LOG_LEVEL);

struct sink_fake_config {
  struct zstreamer_node_config common;
};

struct sink_fake_data {
  struct zstreamer_node_data common;
};

static int sink_fake_process(const struct device *dev, struct net_buf *buf) {
  ARG_UNUSED(dev);
  ARG_UNUSED(buf);

  return 0;
}

static const struct zstreamer_node_driver_api sink_fake_api = {
    .process = sink_fake_process,
};

#define SINK_FAKE_DEFINE(inst)                                                 \
  Z_ZSTREAMER_NODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                   \
  static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                    \
                               DT_INST_PROP(inst, thread_stack_size));         \
  static struct sink_fake_data sink_fake_data_##inst = {                       \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(inst, zstreamer_node_stack_##inst), \
  };                                                                           \
  static const struct sink_fake_config sink_fake_config_##inst = {             \
      .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(                                 \
          inst, DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),      \
          DT_INST_PROP(inst, thread_priority))},                               \
  };                                                                           \
  Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, NULL)                             \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_init_##inst, NULL,                \
                        &sink_fake_data_##inst, &sink_fake_config_##inst,      \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &sink_fake_api);

DT_INST_FOREACH_STATUS_OKAY(SINK_FAKE_DEFINE)
