/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_fake_sink

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(fake_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

static int fake_sink_process(const struct device *dev, struct net_buf *buf) {
  ARG_UNUSED(dev);
  ARG_UNUSED(buf);

  return 0;
}

static const struct zstreamer_node_driver_api fake_sink_api = {
    .process = fake_sink_process,
};

#define FAKE_SINK_DEFINE(inst)                                                 \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct zstreamer_sink_data fake_sink_data_##inst =                    \
      ZSTREAMER_SINK_DATA_INIT(inst);                                          \
  static const struct zstreamer_sink_config fake_sink_config_##inst =          \
      ZSTREAMER_SINK_CONFIG_INIT(inst);                                        \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_sink_common_init, NULL,                \
                        &fake_sink_data_##inst, &fake_sink_config_##inst,      \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &fake_sink_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_SINK_DEFINE)
