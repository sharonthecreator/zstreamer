/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_count_sink

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zstreamer/sink.h>

#include "count_sink.h"

LOG_MODULE_REGISTER(count_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct count_sink_config {
  struct zstreamer_sink_config common;
};

struct count_sink_data {
  struct zstreamer_sink_data common;
  atomic_t buf_count;
  atomic_t byte_count;
};

static int count_sink_process(const struct device *dev, struct net_buf *buf) {
  struct count_sink_data *data = dev->data;

  atomic_inc(&data->buf_count);
  atomic_add(&data->byte_count, buf->len);

  return 0;
}

uint32_t count_sink_get_buf_count(const struct device *dev) {
  struct count_sink_data *data = dev->data;

  return (uint32_t)atomic_get(&data->buf_count);
}

uint32_t count_sink_get_byte_count(const struct device *dev) {
  struct count_sink_data *data = dev->data;

  return (uint32_t)atomic_get(&data->byte_count);
}

void count_sink_reset(const struct device *dev) {
  struct count_sink_data *data = dev->data;

  atomic_set(&data->buf_count, 0);
  atomic_set(&data->byte_count, 0);
}

static const struct zstreamer_sink_driver_api count_sink_api = {
    .process = count_sink_process,
};

#define COUNT_SINK_DEFINE(inst)                                                \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct count_sink_data count_sink_data_##inst = {                     \
      .common = Z_ZSTREAMER_SINK_DATA_INIT(                                    \
          inst, zstreamer_sink_stack_##inst),                                  \
  };                                                                           \
  static const struct count_sink_config count_sink_config_##inst = {           \
      .common = {Z_ZSTREAMER_SINK_CONFIG_INIT(                                 \
          DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),            \
          DT_INST_PROP(inst, thread_priority))},                               \
  };                                                                           \
  ZSTREAMER_SINK_DT_INST_DEFINE(inst, NULL, &count_sink_data_##inst,           \
                                &count_sink_config_##inst, &count_sink_api);

DT_INST_FOREACH_STATUS_OKAY(COUNT_SINK_DEFINE)
