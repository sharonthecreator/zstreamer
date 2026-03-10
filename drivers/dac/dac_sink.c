/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * DAC sink streaming node driver.
 *
 * Receives buffers of interleaved samples and writes each frame to one
 * or more DAC channels using Zephyr's portable DAC API.  The sample
 * width (8- or 16-bit) is selected at compile time per instance based
 * on the dac-resolution DTS property.
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
  const uint8_t *channel_ids;
  uint8_t num_channels;
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

  for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {
    struct dac_channel_cfg ch_cfg = {
        .channel_id = cfg->channel_ids[ch],
        .resolution = cfg->dac_resolution,
        .buffered = false,
    };

    int ret = dac_channel_setup(cfg->dac_dev, &ch_cfg);
    if (ret < 0) {
      LOG_ERR("dac_channel_setup ch%u failed: %d", cfg->channel_ids[ch], ret);
      return ret;
    }
  }

  LOG_INF("DAC sink %s: %u ch, %u-bit", dev->name, cfg->num_channels,
          cfg->dac_resolution);

  return zstreamer_sink_common_init(dev);
}

static int dac_sink_process(const struct device *dev, struct net_buf *buf) {
  const struct dac_sink_config *cfg = dev->config;
  /* Input sample width: 1 byte for <= 8-bit, 2 bytes otherwise. */
  size_t sample_bytes = DIV_ROUND_UP(cfg->dac_resolution, 8);
  size_t frame_size = cfg->num_channels * sample_bytes;
  size_t num_samples = buf->len / frame_size;

  for (size_t s = 0; s < num_samples; s++) {
    for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {
      uint32_t value = 0;
      /* Copy the sample for the specific adc channel. */
      size_t offset = (s * cfg->num_channels + ch) * sample_bytes;
      memcpy(&value, buf->data + offset, sample_bytes);

      /* Mask value to the channel's resolution. */
      uint32_t mask = BIT_MASK(cfg->dac_resolution);
      if (value > mask) {
        value &= mask;
        LOG_DBG("ch%u sample %zu: value %u masked to %u-bit",
                cfg->channel_ids[ch], s, value, cfg->dac_resolution);
      }

      int ret = dac_write_value(cfg->dac_dev, cfg->channel_ids[ch], value);
      if (ret < 0) {
        LOG_ERR("dac_write_value ch%u failed: %d", cfg->channel_ids[ch], ret);
        return ret;
      }
    }
  }

  return 0;
}

static const struct zstreamer_node_driver_api dac_sink_api = {
    .process = dac_sink_process,
};

#define DAC_SINK_CHANNEL_ELEM(node_id, prop, idx)                              \
  DT_PROP_BY_IDX(node_id, prop, idx),

#define DAC_SINK_DEFINE(inst)                                                  \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
                                                                               \
  static const uint8_t dac_sink_channels_##inst[] = {                          \
      DT_INST_FOREACH_PROP_ELEM(inst, dac_channel_ids,                         \
                                DAC_SINK_CHANNEL_ELEM)};                       \
                                                                               \
  static struct dac_sink_data dac_sink_data_##inst = {                         \
      .common = ZSTREAMER_SINK_DATA_INIT(inst),                                \
  };                                                                           \
  static const struct dac_sink_config dac_sink_config_##inst = {               \
      .common = ZSTREAMER_SINK_CONFIG_INIT(inst),                              \
      .dac_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, dac_device)),             \
      .channel_ids = dac_sink_channels_##inst,                                 \
      .num_channels = ARRAY_SIZE(dac_sink_channels_##inst),                    \
      .dac_resolution = DT_INST_PROP(inst, dac_resolution),                    \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, dac_sink_init, NULL, &dac_sink_data_##inst,      \
                        &dac_sink_config_##inst, POST_KERNEL,                  \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &dac_sink_api);

DT_INST_FOREACH_STATUS_OKAY(DAC_SINK_DEFINE)
