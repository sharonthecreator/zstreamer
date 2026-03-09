/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_odd_filter

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/filter.h>

LOG_MODULE_REGISTER(odd_filter, CONFIG_ZSTREAMER_LOG_LEVEL);

static int odd_filter_process(const struct device *dev, struct net_buf *buf) {
  ARG_UNUSED(dev);

  if (buf->len == 0) {
    return 0;
  }

  return (buf->data[0] & 1) != 0 ? 1 : 0;
}

static const struct zstreamer_node_driver_api odd_filter_api = {
    .process = odd_filter_process,
};

#define ODD_FILTER_DEFINE(inst)                                                \
  ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst);                                   \
  static struct zstreamer_filter_data odd_filter_data_##inst =                 \
      ZSTREAMER_FILTER_DATA_INIT(inst);                                        \
  static const struct zstreamer_filter_config odd_filter_config_##inst =       \
      ZSTREAMER_FILTER_CONFIG_INIT(inst);                                      \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_filter_common_init, NULL,              \
                        &odd_filter_data_##inst, &odd_filter_config_##inst,    \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &odd_filter_api);

DT_INST_FOREACH_STATUS_OKAY(ODD_FILTER_DEFINE)
