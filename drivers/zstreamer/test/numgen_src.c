/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_numgen_src

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(numgen_src, CONFIG_ZSTREAMER_LOG_LEVEL);

struct numgen_src_data {
  struct zstreamer_source_data common;
  uint8_t counter;
};

static int numgen_src_process(const struct device *dev, struct net_buf *buf) {
  struct numgen_src_data *data = dev->data;

  while (net_buf_tailroom(buf) > 0) {
    net_buf_add_u8(buf, data->counter++);
  }

  /* Allow simulated time to advance on native_sim / POSIX targets. */
  k_sleep(K_MSEC(1));

  return 0;
}

static const struct zstreamer_node_driver_api numgen_src_api = {
    .process = numgen_src_process,
};

#define NUMGEN_SRC_DEFINE(inst)                                                \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
  static struct numgen_src_data numgen_src_data_##inst = {                     \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst), .counter = 0};               \
  static const struct zstreamer_source_config numgen_src_config_##inst =       \
      ZSTREAMER_SOURCE_CONFIG_INIT(inst);                                      \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,                \
                        &numgen_src_data_##inst, &numgen_src_config_##inst,    \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &numgen_src_api);

DT_INST_FOREACH_STATUS_OKAY(NUMGEN_SRC_DEFINE)
