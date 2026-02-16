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

LOG_MODULE_REGISTER(src_uart, CONFIG_ZSTREAMER_LOG_LEVEL);

#if defined(CONFIG_UART_ASYNC_API)
#ifndef CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE
#define CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE 256
#endif
#endif

struct src_uart_config {
  struct zstreamer_source_config common;
  const struct device *uart_dev;
};

struct src_uart_data {
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

static void src_uart_rx_handler(void *user_data, const uint8_t *data,
                                size_t len) {
  struct src_uart_data *drv_data = user_data;

  drv_data->rx_data = data;
  drv_data->rx_len = len;
  k_sem_give(&drv_data->rx_sem);
}

static int src_uart_open_dma(const struct device *dev) {
  const struct src_uart_config *cfg = dev->config;
  struct src_uart_data *data = dev->data;
  int ret;

  ret = uart_dma_context_register_rx(cfg->uart_dev, src_uart_rx_handler, data);
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

static int src_uart_close_dma(const struct device *dev) {
  const struct src_uart_config *cfg = dev->config;
  struct src_uart_data *data = dev->data;

  if (data->dma_enabled) {
    uart_dma_context_rx_disable(cfg->uart_dev);
    uart_dma_context_unregister_rx(cfg->uart_dev);
    data->dma_enabled = false;
  }
  return 0;
}

static int src_uart_process_dma(const struct device *dev, struct net_buf *buf) {
  struct src_uart_data *data = dev->data;

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

static int src_uart_process_poll(const struct device *dev,
                                 struct net_buf *buf) {
  const struct src_uart_config *cfg = dev->config;
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

static int src_uart_process(const struct device *dev, struct net_buf *buf) {
#if defined(CONFIG_UART_ASYNC_API)
  struct src_uart_data *data = dev->data;

  if (data->dma_enabled) {
    return src_uart_process_dma(dev, buf);
  }
#endif
  return src_uart_process_poll(dev, buf);
}

#if defined(CONFIG_UART_ASYNC_API)
static int src_uart_open(const struct device *dev) {
  const struct src_uart_config *cfg = dev->config;
  int ret;

  /* Try to enable DMA, fall back to polling if it fails. */
  ret = src_uart_open_dma(dev);
  if (ret != 0) {
    LOG_INF("DMA not available for %s, using polling", cfg->uart_dev->name);
  }
  return 0;
}

static int src_uart_close(const struct device *dev) {
  return src_uart_close_dma(dev);
}
#endif

static const struct zstreamer_source_driver_api src_uart_api = {
#if defined(CONFIG_UART_ASYNC_API)
    .open = src_uart_open,
    .close = src_uart_close,
#endif
    .generate = src_uart_process,
};

#if defined(CONFIG_UART_ASYNC_API)
static int src_uart_init(const struct device *dev) {
  struct src_uart_data *data = dev->data;

  k_sem_init(&data->rx_sem, 0, 1);
  return 0;
}
#define SRC_UART_INIT_FN src_uart_init
#else
#define SRC_UART_INIT_FN NULL
#endif

#define SRC_UART_DEFINE(inst)                                                  \
  static struct src_uart_data src_uart_data_##inst = {                         \
      .common = Z_ZSTREAMER_SOURCE_DATA_INIT(                                  \
          inst, zstreamer_source_stack_##inst),                                \
  };                                                                           \
  static const struct src_uart_config src_uart_config_##inst = {               \
      .common = {Z_ZSTREAMER_SOURCE_CONFIG_INIT(                               \
          inst, DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),      \
          DT_INST_PROP(inst, thread_priority))},                               \
      .uart_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart_device)),           \
  };                                                                           \
  ZSTREAMER_SOURCE_DT_INST_DEFINE(inst, SRC_UART_INIT_FN,                      \
                                  &src_uart_data_##inst,                       \
                                  &src_uart_config_##inst, &src_uart_api);

DT_INST_FOREACH_STATUS_OKAY(SRC_UART_DEFINE)
