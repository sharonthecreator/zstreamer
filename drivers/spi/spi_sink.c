/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_spi_sink

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(spi_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct spi_sink_config {
  struct zstreamer_sink_config common;
  struct spi_dt_spec spi;
};

struct spi_sink_data {
  struct zstreamer_sink_data common;
#if defined(CONFIG_SPI_ASYNC)
  struct k_poll_signal sig;
  struct k_poll_event evt;
  bool async_enabled;
#endif
};

#if defined(CONFIG_SPI_ASYNC)

static int spi_sink_process_async(const struct device *dev,
                                  struct net_buf *buf) {
  const struct spi_sink_config *cfg = dev->config;
  struct spi_sink_data *data = dev->data;
  int ret, result;

  if (buf->len == 0) {
    return 0;
  }

  struct spi_buf tx_buf = {
      .buf = buf->data,
      .len = buf->len,
  };
  struct spi_buf_set tx_bufs = {
      .buffers = &tx_buf,
      .count = 1,
  };

  k_poll_signal_reset(&data->sig);
  data->evt.state = K_POLL_STATE_NOT_READY;

  ret = spi_write_signal(cfg->spi.bus, &cfg->spi.config, &tx_bufs, &data->sig);
  if (ret < 0) {
    LOG_ERR("SPI async write failed: %d", ret);
    return ret;
  }

  ret = k_poll(&data->evt, 1, K_FOREVER);
  if (ret < 0) {
    LOG_ERR("SPI TX poll failed: %d", ret);
    return ret;
  }

  result = data->sig.result;
  if (result < 0) {
    LOG_ERR("SPI TX error: %d", result);
  }

  return result;
}

static int spi_sink_open_async(const struct device *dev) {
  const struct spi_sink_config *cfg = dev->config;
  struct spi_sink_data *data = dev->data;
  uint8_t dummy = 0;
  struct spi_buf test_buf = {.buf = &dummy, .len = 1};
  struct spi_buf_set test_bufs = {.buffers = &test_buf, .count = 1};
  int ret;

  k_poll_signal_reset(&data->sig);
  data->evt.state = K_POLL_STATE_NOT_READY;

  ret =
      spi_write_signal(cfg->spi.bus, &cfg->spi.config, &test_bufs, &data->sig);
  if (ret == -ENOTSUP) {
    LOG_INF("SPI async not supported, using polling");
    return -ENOTSUP;
  }
  if (ret < 0) {
    LOG_WRN("SPI async probe failed: %d, using polling", ret);
    return ret;
  }

  /* Wait for probe transfer to complete. */
  k_poll(&data->evt, 1, K_MSEC(100));

  data->async_enabled = true;
  LOG_DBG("SPI async TX enabled");
  return 0;
}

#endif /* CONFIG_SPI_ASYNC */

static int spi_sink_process_poll(const struct device *dev,
                                 struct net_buf *buf) {
  const struct spi_sink_config *cfg = dev->config;

  if (buf->len == 0) {
    return 0;
  }

  struct spi_buf tx_buf = {
      .buf = buf->data,
      .len = buf->len,
  };
  struct spi_buf_set tx_bufs = {
      .buffers = &tx_buf,
      .count = 1,
  };

  return spi_write_dt(&cfg->spi, &tx_bufs);
}

static int spi_sink_process(const struct device *dev, struct net_buf *buf) {
#if defined(CONFIG_SPI_ASYNC)
  struct spi_sink_data *data = dev->data;

  if (data->async_enabled) {
    return spi_sink_process_async(dev, buf);
  }
#endif
  return spi_sink_process_poll(dev, buf);
}

static const struct zstreamer_node_driver_api spi_sink_api = {
    .process = spi_sink_process,
};

static int spi_sink_init(const struct device *dev) {
#if defined(CONFIG_SPI_ASYNC)
  struct spi_sink_data *data = dev->data;

  k_poll_signal_init(&data->sig);
  k_poll_event_init(&data->evt, K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY,
                    &data->sig);
  spi_sink_open_async(dev);
#endif
  return zstreamer_sink_common_init(dev);
}

#define SPI_DEV_NODE(inst) DT_INST_PHANDLE(inst, spi_device)

#define SPI_SINK_DEFINE(inst)                                                  \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct spi_sink_data spi_sink_data_##inst = {                         \
      .common = ZSTREAMER_SINK_DATA_INIT(inst),                                \
  };                                                                           \
  static const struct spi_sink_config spi_sink_config_##inst = {               \
      .common = ZSTREAMER_SINK_CONFIG_INIT(inst),                              \
      .spi = SPI_DT_SPEC_GET(SPI_DEV_NODE(inst),                               \
                             SPI_OP_MODE_MASTER | SPI_WORD_SET(8)),            \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, spi_sink_init, NULL, &spi_sink_data_##inst,      \
                        &spi_sink_config_##inst, POST_KERNEL,                  \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &spi_sink_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_SINK_DEFINE)
