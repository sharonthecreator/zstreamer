/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_lora_src

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(lora_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#define MAX_LORA_RECV_SIZE 255

struct lora_src_config {
  struct zstreamer_source_config common;
  const struct device *lora_dev;
  struct lora_modem_config modem_cfg;
};

struct lora_src_data {
  struct zstreamer_source_data common;
  bool configured;
  uint8_t rx_buf[MAX_LORA_RECV_SIZE];
};

static int lora_src_configure(const struct device *dev) {
  const struct lora_src_config *cfg = dev->config;
  struct lora_src_data *data = dev->data;

  if (data->configured) {
    return 0;
  }

  if (!device_is_ready(cfg->lora_dev)) {
    LOG_ERR("LoRa device not ready");
    return -ENODEV;
  }

  int ret =
      lora_config(cfg->lora_dev, (struct lora_modem_config *)&cfg->modem_cfg);

  if (ret < 0) {
    LOG_ERR("lora_config failed: %d", ret);
    return ret;
  }

  LOG_INF("LoRa source %s: %u Hz, SF%u, BW%u", dev->name,
          cfg->modem_cfg.frequency, cfg->modem_cfg.datarate,
          cfg->modem_cfg.bandwidth);

  data->configured = true;
  return 0;
}

static int lora_src_process(const struct device *dev, struct net_buf *buf) {
  const struct lora_src_config *cfg = dev->config;
  int16_t rssi;
  int8_t snr;
  int ret;

  ret = lora_src_configure(dev);
  if (ret < 0) {
    return ret;
  }

  struct lora_src_data *data = dev->data;
  uint8_t max_len = MIN(net_buf_tailroom(buf), MAX_LORA_RECV_SIZE);

  int len =
      lora_recv(cfg->lora_dev, data->rx_buf, max_len, K_FOREVER, &rssi, &snr);

  if (len < 0) {
    LOG_ERR("lora_recv failed: %d", len);
    return len;
  }

  if (len > 0) {
    memcpy(net_buf_add(buf, len), data->rx_buf, len);
    LOG_DBG("RX %d bytes, RSSI %d dBm, SNR %d dB", len, rssi, snr);
  }

  return 0;
}

static const struct zstreamer_node_driver_api lora_src_api = {
    .process = lora_src_process,
};

#define LORA_SRC_DEFINE(inst)                                                  \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
  static struct lora_src_data lora_src_data_##inst = {                         \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                              \
  };                                                                           \
  static const struct lora_src_config lora_src_config_##inst = {               \
      .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                            \
      .lora_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, lora_device)),           \
      .modem_cfg =                                                             \
          {                                                                    \
              .frequency = DT_INST_PROP(inst, frequency),                      \
              .bandwidth = DT_INST_PROP(inst, bandwidth),                      \
              .datarate = DT_INST_PROP(inst, spreading_factor),                \
              .coding_rate = DT_INST_PROP(inst, coding_rate),                  \
              .preamble_len = DT_INST_PROP(inst, preamble_length),             \
              .tx_power = DT_INST_PROP(inst, tx_power),                        \
              .tx = false,                                                     \
          },                                                                   \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_source_common_init, NULL,              \
                        &lora_src_data_##inst, &lora_src_config_##inst,        \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &lora_src_api);

DT_INST_FOREACH_STATUS_OKAY(LORA_SRC_DEFINE)
