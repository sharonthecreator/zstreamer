/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_fake_sink

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(fake_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct fake_sink_config {
  struct zstreamer_sink_config common;
};

struct fake_sink_data {
  struct zstreamer_sink_data common;
};

static int fake_sink_process(const struct device *dev, struct net_buf *buf) {
  ARG_UNUSED(dev);
  ARG_UNUSED(buf);

  return 0;
}

static const struct zstreamer_sink_driver_api fake_sink_api = {
    .process = fake_sink_process,
};

#define FAKE_SINK_DEFINE(inst)                                                 \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct fake_sink_data fake_sink_data_##inst = {                       \
      .common = Z_ZSTREAMER_SINK_DATA_INIT(                                    \
          inst, zstreamer_sink_stack_##inst),                                  \
  };                                                                           \
  static const struct fake_sink_config fake_sink_config_##inst = {             \
      .common = {Z_ZSTREAMER_SINK_CONFIG_INIT(                                 \
          DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),            \
          DT_INST_PROP(inst, thread_priority))},                               \
  };                                                                           \
  ZSTREAMER_SINK_DT_INST_DEFINE(inst, NULL, &fake_sink_data_##inst,            \
                                &fake_sink_config_##inst, &fake_sink_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_SINK_DEFINE)
