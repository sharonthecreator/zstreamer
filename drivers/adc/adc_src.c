/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC source streaming node driver.
 *
 * Captures ADC samples using Zephyr's portable ADC API.  Each call to
 * process() performs blocking adc_read() calls for all configured
 * channels, packs the results into a net_buf, and sleeps to approximate
 * the requested sample rate.
 *
 * Sample width (8-bit or 16-bit output) is selected at init time based on
 * the first channel's resolution.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(adc_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#define DT_DRV_COMPAT zstreamer_adc_src

struct adc_src_config {
  struct zstreamer_source_config common;
  const struct adc_dt_spec *channels;
  uint8_t num_channels;
  uint32_t sample_rate_hz;
  uint16_t samples;
};

struct adc_src_data {
  struct zstreamer_source_data common;
};

static int adc_src_process(const struct device *dev, struct net_buf *buf) {
  const struct adc_src_config *cfg = dev->config;
  /* Output sample width: 1 byte for <= 8-bit, 2 bytes otherwise. */
  size_t sample_bytes = (cfg->channels[0].resolution <= 8) ? 1 : 2;
  size_t frame_size = cfg->num_channels * sample_bytes;
  uint8_t *out = net_buf_add(buf, frame_size * cfg->samples);

  for (uint16_t s = 0; s < cfg->samples; s++) {
    for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {
      const struct adc_dt_spec *spec = &cfg->channels[ch];
      /* ADC API always writes uint16_t values. */
      uint16_t adc_value = 0;

      struct adc_sequence seq = {
          .channels = BIT(spec->channel_id),
          .buffer = &adc_value,
          .buffer_size = sizeof(adc_value),
          .resolution = spec->resolution,
          .oversampling = spec->oversampling,
      };

      int ret = adc_read(spec->dev, &seq);
      if (ret < 0) {
        LOG_ERR("adc_read ch%u failed: %d", ch, ret);
        return ret;
      }

      size_t offset = (s * cfg->num_channels + ch) * sample_bytes;
      memcpy(out + offset, &adc_value, sample_bytes);
    }
  }

  /* Pace output to approximate the requested sample rate. */
  uint32_t interval_us =
      (uint32_t)(((uint64_t)cfg->samples * 1000000U) / cfg->sample_rate_hz);
  k_usleep(interval_us);

  return 0;
}

static int adc_src_init(const struct device *dev) {
  const struct adc_src_config *cfg = dev->config;
  int ret;

  for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {
    const struct adc_dt_spec *spec = &cfg->channels[ch];

    if (!adc_is_ready_dt(spec)) {
      LOG_ERR("ADC device for channel %u not ready", ch);
      return -ENODEV;
    }

    ret = adc_channel_setup_dt(spec);
    if (ret < 0) {
      LOG_ERR("adc_channel_setup ch%u failed: %d", ch, ret);
      return ret;
    }
  }

  LOG_INF("ADC source %s: %u Hz, %u ch, %u samples/buf, %u-bit", dev->name,
          cfg->sample_rate_hz, cfg->num_channels, cfg->samples,
          cfg->channels[0].resolution);

  return zstreamer_source_common_init(dev);
}

static const struct zstreamer_node_driver_api adc_src_api = {
    .process = adc_src_process,
};

/* --- Per-channel ADC specs from io-channels --- */

#define ADC_SRC_CHANNEL_SPEC(node_id, prop, idx)                               \
  ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

/* Resolve the first io-channel's DT node to read zephyr,resolution. */
#define ADC_SRC_FIRST_CH_NODE(inst)                                            \
  ADC_CHANNEL_DT_NODE(DT_IO_CHANNELS_CTLR_BY_IDX(DT_DRV_INST(inst), 0),        \
                      DT_IO_CHANNELS_INPUT_BY_IDX(DT_DRV_INST(inst), 0))

#define ADC_SRC_SAMPLE_BYTES(inst)                                             \
  ((DT_PROP(ADC_SRC_FIRST_CH_NODE(inst), zephyr_resolution) <= 8) ? 1 : 2)

#define ADC_SRC_DEFINE(inst)                                                   \
  BUILD_ASSERT(DT_INST_PROP(inst, samples) * ADC_SRC_SAMPLE_BYTES(inst) *      \
                       DT_PROP_LEN(DT_DRV_INST(inst), io_channels) <=          \
                   DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_size),         \
               "adc-src: samples * channels * sample_bytes exceeds graph "     \
               "buffer-size");                                                 \
                                                                               \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
                                                                               \
  static const struct adc_dt_spec adc_src_channels_##inst[] = {                \
      DT_INST_FOREACH_PROP_ELEM(inst, io_channels, ADC_SRC_CHANNEL_SPEC)};     \
                                                                               \
  static struct adc_src_data adc_src_data_##inst = {                           \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                              \
  };                                                                           \
  static const struct adc_src_config adc_src_config_##inst = {                 \
      .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                            \
      .channels = adc_src_channels_##inst,                                     \
      .num_channels = ARRAY_SIZE(adc_src_channels_##inst),                     \
      .sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                    \
      .samples = DT_INST_PROP(inst, samples),                                  \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, adc_src_init, NULL, &adc_src_data_##inst,        \
                        &adc_src_config_##inst, POST_KERNEL,                   \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &adc_src_api);

DT_INST_FOREACH_STATUS_OKAY(ADC_SRC_DEFINE)
