/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_lora_sink

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(lora_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct lora_sink_config {
  struct zstreamer_sink_config common;
  const struct device *lora_dev;
  uint32_t frequency;
  enum lora_signal_bandwidth bandwidth;
  enum lora_datarate spreading_factor;
  enum lora_coding_rate coding_rate;
  uint16_t preamble_len;
  int8_t tx_power;
};

struct lora_sink_data {
  struct zstreamer_sink_data common;
  bool configured;
};

static int lora_sink_configure(const struct device *dev) {
  const struct lora_sink_config *cfg = dev->config;
  struct lora_sink_data *data = dev->data;

  if (data->configured) {
    return 0;
  }

  if (!device_is_ready(cfg->lora_dev)) {
    LOG_ERR("LoRa device not ready");
    return -ENODEV;
  }

  struct lora_modem_config modem_cfg = {
      .frequency = cfg->frequency,
      .bandwidth = cfg->bandwidth,
      .datarate = cfg->spreading_factor,
      .coding_rate = cfg->coding_rate,
      .preamble_len = cfg->preamble_len,
      .tx_power = cfg->tx_power,
      .tx = true,
      .iq_inverted = false,
      .public_network = false,
  };

  int ret = lora_config(cfg->lora_dev, &modem_cfg);

  if (ret < 0) {
    LOG_ERR("lora_config failed: %d", ret);
    return ret;
  }

  LOG_INF("LoRa sink %s: %u Hz, SF%u, BW%u, TX %d dBm", dev->name,
          cfg->frequency, cfg->spreading_factor, cfg->bandwidth,
          cfg->tx_power);

  data->configured = true;
  return 0;
}

static int lora_sink_process(const struct device *dev, struct net_buf *buf) {
  const struct lora_sink_config *cfg = dev->config;
  int ret;

  ret = lora_sink_configure(dev);
  if (ret < 0) {
    return ret;
  }

  if (buf->len == 0) {
    return 0;
  }

  ret = lora_send(cfg->lora_dev, buf->data, buf->len);

  if (ret < 0) {
    LOG_ERR("lora_send failed: %d", ret);
  }

  return ret;
}

static const struct zstreamer_node_driver_api lora_sink_api = {
    .process = lora_sink_process,
};

#define LORA_SINK_DEFINE(inst)                                                 \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct lora_sink_data lora_sink_data_##inst = {                       \
      .common = ZSTREAMER_SINK_DATA_INIT(inst),                                \
  };                                                                           \
  static const struct lora_sink_config lora_sink_config_##inst = {             \
      .common = ZSTREAMER_SINK_CONFIG_INIT(inst),                              \
      .lora_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, lora_device)),           \
      .frequency = DT_INST_PROP(inst, frequency),                              \
      .bandwidth = DT_INST_PROP(inst, bandwidth),                              \
      .spreading_factor = DT_INST_PROP(inst, spreading_factor),                \
      .coding_rate = DT_INST_PROP(inst, coding_rate),                          \
      .preamble_len = DT_INST_PROP(inst, preamble_length),                     \
      .tx_power = DT_INST_PROP(inst, tx_power),                                \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_sink_common_init, NULL,                \
                        &lora_sink_data_##inst, &lora_sink_config_##inst,       \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &lora_sink_api);

DT_INST_FOREACH_STATUS_OKAY(LORA_SINK_DEFINE)
