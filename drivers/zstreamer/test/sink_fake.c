/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_fake_sink

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(sink_fake, CONFIG_ZSTREAMER_LOG_LEVEL);

struct sink_fake_config {
  struct zstreamer_sink_config common;
};

struct sink_fake_data {
  struct zstreamer_sink_data common;
};

static int sink_fake_process(const struct device *dev, struct net_buf *buf) {
  ARG_UNUSED(dev);
  ARG_UNUSED(buf);

  return 0;
}

static const struct zstreamer_sink_driver_api sink_fake_api = {
    .process = sink_fake_process,
};

#define SINK_FAKE_DEFINE(inst)                                                 \
  static struct sink_fake_data sink_fake_data_##inst = {                       \
      .common = Z_ZSTREAMER_SINK_DATA_INIT(                                    \
          inst, zstreamer_sink_stack_##inst),                                  \
  };                                                                           \
  static const struct sink_fake_config sink_fake_config_##inst = {             \
      .common = {Z_ZSTREAMER_SINK_CONFIG_INIT(                                 \
          DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),            \
          DT_INST_PROP(inst, thread_priority))},                               \
  };                                                                           \
  ZSTREAMER_SINK_DT_INST_DEFINE(inst, NULL, &sink_fake_data_##inst,            \
                                &sink_fake_config_##inst, &sink_fake_api);

DT_INST_FOREACH_STATUS_OKAY(SINK_FAKE_DEFINE)
