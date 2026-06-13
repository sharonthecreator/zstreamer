/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_uart_sink

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

#if defined(CONFIG_UART_ASYNC_API)
#include "uart_dma_context.h"
#endif

LOG_MODULE_REGISTER(uart_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct uart_sink_config {
  struct zstreamer_sink_config common;
  const struct device *uart_dev;
};

struct uart_sink_data {
  struct zstreamer_sink_data common;
#if defined(CONFIG_UART_ASYNC_API)
  struct k_sem tx_sem;
  int tx_err;
  bool dma_enabled;
#endif
};

#if defined(CONFIG_UART_ASYNC_API)

static void uart_sink_tx_handler(void *user_data, int err) {
  struct uart_sink_data *data = user_data;

  data->tx_err = err;
  k_sem_give(&data->tx_sem);
}

static int uart_sink_open_dma(const struct device *dev) {
  const struct uart_sink_config *cfg = dev->config;
  struct uart_sink_data *data = dev->data;
  int ret;

  ret = uart_dma_context_register_tx(cfg->uart_dev, uart_sink_tx_handler, data);
  if (ret != 0) {
    LOG_ERR("Failed to register TX handler: %d", ret);
    return ret;
  }

  data->dma_enabled = true;
  LOG_DBG("DMA TX enabled");
  return 0;
}

static int uart_sink_process_dma(const struct device *dev,
                                 struct net_buf *buf) {
  const struct uart_sink_config *cfg = dev->config;
  struct uart_sink_data *data = dev->data;
  int ret;

  if (buf->len == 0) {
    return 0;
  }

  ret = uart_dma_context_tx(cfg->uart_dev, buf->data, buf->len);
  if (ret != 0) {
    LOG_ERR("DMA TX failed: %d", ret);
    return ret;
  }

  /* Wait for TX completion. */
  k_sem_take(&data->tx_sem, K_FOREVER);

  return data->tx_err;
}

#endif /* CONFIG_UART_ASYNC_API */

static int uart_sink_process_poll(const struct device *dev,
                                  struct net_buf *buf) {
  const struct uart_sink_config *cfg = dev->config;

  for (uint16_t i = 0; i < buf->len; i++) {
    uart_poll_out(cfg->uart_dev, buf->data[i]);
  }

  return 0;
}

static int uart_sink_process(const struct device *dev, struct net_buf *buf) {
  LOG_DBG("uart_sink TX %u bytes", buf->len);
  LOG_HEXDUMP_DBG(buf->data, buf->len, "uart_sink TX data");

#if defined(CONFIG_UART_ASYNC_API)
  struct uart_sink_data *data = dev->data;

  if (data->dma_enabled) {
    return uart_sink_process_dma(dev, buf);
  }
#endif
  return uart_sink_process_poll(dev, buf);
}

static const struct zstreamer_node_driver_api uart_sink_api = {
    .process = uart_sink_process,
};

static int uart_sink_init(const struct device *dev) {
#if defined(CONFIG_UART_ASYNC_API)
  const struct uart_sink_config *cfg = dev->config;
  struct uart_sink_data *data = dev->data;
  int ret;

  k_sem_init(&data->tx_sem, 0, 1);

  /* Try to enable DMA, fall back to polling if it fails. */
  ret = uart_sink_open_dma(dev);
  if (ret != 0) {
    LOG_INF("DMA not available for %s, using polling", cfg->uart_dev->name);
  }
#endif
  return zstreamer_sink_common_init(dev);
}

#define UART_SINK_DEFINE(inst)                                                 \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct uart_sink_data uart_sink_data_##inst = {                       \
      .common = ZSTREAMER_SINK_DATA_INIT(inst),                                \
  };                                                                           \
  static const struct uart_sink_config uart_sink_config_##inst = {             \
      .common = ZSTREAMER_SINK_CONFIG_INIT(inst),                              \
      .uart_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart_device)),           \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, uart_sink_init, NULL, &uart_sink_data_##inst,    \
                        &uart_sink_config_##inst, POST_KERNEL,                 \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &uart_sink_api);

DT_INST_FOREACH_STATUS_OKAY(UART_SINK_DEFINE)
