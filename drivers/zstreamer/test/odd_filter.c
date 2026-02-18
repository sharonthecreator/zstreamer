/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_odd_filter

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/filter.h>

LOG_MODULE_REGISTER(odd_filter, CONFIG_ZSTREAMER_LOG_LEVEL);

struct odd_filter_config {
  struct zstreamer_filter_config common;
};

struct odd_filter_data {
  struct zstreamer_filter_data common;
};

static bool odd_filter_filter(const struct device *dev, struct net_buf *buf) {
  ARG_UNUSED(dev);

  if (buf->len == 0) {
    return false;
  }

  return (buf->data[0] & 1) != 0;
}

static const struct zstreamer_filter_driver_api odd_filter_api = {
    .filter = odd_filter_filter,
};

#define ODD_FILTER_DEFINE(inst)                                                \
  ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst);                                   \
  static struct odd_filter_data odd_filter_data_##inst = {                     \
      .common = Z_ZSTREAMER_FILTER_DATA_INIT(                                  \
          inst, zstreamer_filter_stack_##inst),                                \
  };                                                                           \
  static const struct odd_filter_config odd_filter_config_##inst = {           \
      .common = {Z_ZSTREAMER_FILTER_CONFIG_INIT(                               \
          inst, DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),      \
          DT_INST_PROP(inst, thread_priority))},                               \
  };                                                                           \
  ZSTREAMER_FILTER_DT_INST_DEFINE(inst, NULL, &odd_filter_data_##inst,         \
                                  &odd_filter_config_##inst,                   \
                                  &odd_filter_api);

DT_INST_FOREACH_STATUS_OKAY(ODD_FILTER_DEFINE)
