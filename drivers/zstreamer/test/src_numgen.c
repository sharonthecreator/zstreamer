/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_numgen_src

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(src_numgen, CONFIG_ZSTREAMER_LOG_LEVEL);

struct src_numgen_config {
  struct zstreamer_source_config common;
};

struct src_numgen_data {
  struct zstreamer_source_data common;
  uint8_t counter;
};

static int src_numgen_process(const struct device *dev, struct net_buf *buf) {
  struct src_numgen_data *data = dev->data;

  while (net_buf_tailroom(buf) > 0) {
    net_buf_add_u8(buf, data->counter++);
  }

  return 0;
}

static const struct zstreamer_source_driver_api src_numgen_api = {
    .generate = src_numgen_process,
};

#define SRC_NUMGEN_DEFINE(inst)                                                \
  static struct src_numgen_data src_numgen_data_##inst = {                     \
      .common = Z_ZSTREAMER_SOURCE_DATA_INIT(                                  \
          inst, zstreamer_source_stack_##inst),                                \
  };                                                                           \
  static const struct src_numgen_config src_numgen_config_##inst = {           \
      .common = {Z_ZSTREAMER_SOURCE_CONFIG_INIT(                               \
          inst, DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),      \
          DT_INST_PROP(inst, thread_priority))},                               \
  };                                                                           \
  ZSTREAMER_SOURCE_DT_INST_DEFINE(inst, NULL, &src_numgen_data_##inst,         \
                                  &src_numgen_config_##inst, &src_numgen_api);

DT_INST_FOREACH_STATUS_OKAY(SRC_NUMGEN_DEFINE)
