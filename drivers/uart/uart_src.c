/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_uart_src

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

#if defined(CONFIG_UART_ASYNC_API)
#include "uart_dma_context.h"
#endif

LOG_MODULE_REGISTER(uart_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#if defined(CONFIG_UART_ASYNC_API)
#ifndef CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE
#define CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE 256
#endif
#endif

struct uart_src_config {
  struct zstreamer_source_config common;
  const struct device *uart_dev;
};

struct uart_src_data {
  struct zstreamer_source_data common;
#if defined(CONFIG_UART_ASYNC_API)
  uint8_t dma_rx_buf[CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE];
  struct k_sem rx_sem;
  const uint8_t *rx_data;
  size_t rx_len;
  bool dma_enabled;
#endif
};

#if defined(CONFIG_UART_ASYNC_API)

static void uart_src_rx_handler(void *user_data, const uint8_t *data,
                                size_t len) {
  struct uart_src_data *drv_data = user_data;

  drv_data->rx_data = data;
  drv_data->rx_len = len;
  k_sem_give(&drv_data->rx_sem);
}

static int uart_src_open_dma(const struct device *dev) {
  const struct uart_src_config *cfg = dev->config;
  struct uart_src_data *data = dev->data;
  int ret;

  ret = uart_dma_context_register_rx(cfg->uart_dev, uart_src_rx_handler, data);
  if (ret != 0) {
    LOG_ERR("Failed to register RX handler: %d", ret);
    return ret;
  }

  ret = uart_dma_context_rx_enable(cfg->uart_dev, data->dma_rx_buf,
                                   sizeof(data->dma_rx_buf), SYS_FOREVER_US);
  if (ret != 0) {
    LOG_ERR("Failed to enable RX: %d", ret);
    uart_dma_context_unregister_rx(cfg->uart_dev);
    return ret;
  }

  data->dma_enabled = true;
  LOG_DBG("DMA RX enabled");
  return 0;
}

static int uart_src_process_dma(const struct device *dev, struct net_buf *buf) {
  struct uart_src_data *data = dev->data;

  /* Wait for RX data with timeout. */
  if (k_sem_take(&data->rx_sem, K_MSEC(100)) != 0) {
    return -EAGAIN;
  }

  if (data->rx_len == 0) {
    return -EAGAIN;
  }

  size_t copy_len = MIN(data->rx_len, net_buf_tailroom(buf));

  net_buf_add_mem(buf, data->rx_data, copy_len);

  return 0;
}

#endif /* CONFIG_UART_ASYNC_API */

static int uart_src_process_poll(const struct device *dev,
                                 struct net_buf *buf) {
  const struct uart_src_config *cfg = dev->config;
  unsigned char c;

  while (net_buf_tailroom(buf) > 0) {
    if (uart_poll_in(cfg->uart_dev, &c) < 0) {
      break;
    }
    net_buf_add_u8(buf, c);
  }

  if (buf->len == 0) {
    k_msleep(1);
    return -EAGAIN;
  }

  return 0;
}

static int uart_src_process(const struct device *dev, struct net_buf *buf) {
#if defined(CONFIG_UART_ASYNC_API)
  struct uart_src_data *data = dev->data;

  if (data->dma_enabled) {
    return uart_src_process_dma(dev, buf);
  }
#endif
  return uart_src_process_poll(dev, buf);
}

static const struct zstreamer_node_driver_api uart_src_api = {
    .process = uart_src_process,
};

static int uart_src_init(const struct device *dev) {
#if defined(CONFIG_UART_ASYNC_API)
  const struct uart_src_config *cfg = dev->config;
  struct uart_src_data *data = dev->data;
  int ret;

  k_sem_init(&data->rx_sem, 0, 1);

  /* Try to enable DMA, fall back to polling if it fails. */
  ret = uart_src_open_dma(dev);
  if (ret != 0) {
    LOG_INF("DMA not available for %s, using polling", cfg->uart_dev->name);
  }
#endif
  return zstreamer_source_common_init(dev);
}

#define UART_SRC_DEFINE(inst)                                                  \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
  static struct uart_src_data uart_src_data_##inst = {                         \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                              \
  };                                                                           \
  static const struct uart_src_config uart_src_config_##inst = {               \
      .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                            \
      .uart_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart_device)),           \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, uart_src_init, NULL, &uart_src_data_##inst,      \
                        &uart_src_config_##inst, POST_KERNEL,                  \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &uart_src_api);

DT_INST_FOREACH_STATUS_OKAY(UART_SRC_DEFINE)
