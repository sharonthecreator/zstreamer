/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * DAC sink streaming node driver.
 *
 * Receives buffers of samples and writes each value to a DAC channel
 * using Zephyr's portable DAC API.  The sample width (8- or 16-bit)
 * is selected at compile time per instance based on the dac-resolution
 * DTS property.
 */

#define DT_DRV_COMPAT zstreamer_dac_sink

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(dac_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct dac_sink_config {
  struct zstreamer_sink_config common;
  const struct device *dac_dev;
  uint8_t dac_channel_id;
  uint8_t dac_resolution;
};

struct dac_sink_data {
  struct zstreamer_sink_data common;
};

static int dac_sink_init(const struct device *dev) {
  const struct dac_sink_config *cfg = dev->config;

  if (!device_is_ready(cfg->dac_dev)) {
    LOG_ERR("DAC device not ready");
    return -ENODEV;
  }

  struct dac_channel_cfg ch_cfg = {
      .channel_id = cfg->dac_channel_id,
      .resolution = cfg->dac_resolution,
      .buffered = false,
  };

  int ret = dac_channel_setup(cfg->dac_dev, &ch_cfg);
  if (ret < 0) {
    LOG_ERR("dac_channel_setup failed: %d", ret);
    return ret;
  }

  LOG_INF("DAC sink %s: ch%u, %u-bit", dev->name, cfg->dac_channel_id,
          cfg->dac_resolution);

  return zstreamer_sink_common_init(dev);
}

static int dac_sink_process(const struct device *dev, struct net_buf *buf) {
  const struct dac_sink_config *cfg = dev->config;
  size_t sample_bytes = DIV_ROUND_UP(cfg->dac_resolution, 8);
  uint32_t mask = BIT_MASK(cfg->dac_resolution);
  size_t num_samples = buf->len / sample_bytes;

  for (size_t i = 0; i < num_samples; i++) {
    uint32_t value;

    if (sample_bytes == 1) {
      value = buf->data[i];
    } else {
      value = ((const uint16_t *)buf->data)[i];
    }

    value &= mask;

    int ret = dac_write_value(cfg->dac_dev, cfg->dac_channel_id, value);

    if (ret < 0) {
      LOG_ERR("dac_write_value failed: %d", ret);
      return ret;
    }
  }

  return 0;
}

static const struct zstreamer_node_driver_api dac_sink_api = {
    .process = dac_sink_process,
};

#define DAC_SINK_DEFINE(inst)                                                  \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct dac_sink_data dac_sink_data_##inst = {                         \
      .common = ZSTREAMER_SINK_DATA_INIT(inst),                                \
  };                                                                           \
  static const struct dac_sink_config dac_sink_config_##inst = {               \
      .common = ZSTREAMER_SINK_CONFIG_INIT(inst),                              \
      .dac_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, dac_device)),             \
      .dac_channel_id = DT_INST_PROP(inst, dac_channel_id),                    \
      .dac_resolution = DT_INST_PROP(inst, dac_resolution),                    \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, dac_sink_init, NULL, &dac_sink_data_##inst,      \
                        &dac_sink_config_##inst, POST_KERNEL,                  \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &dac_sink_api);

DT_INST_FOREACH_STATUS_OKAY(DAC_SINK_DEFINE)
