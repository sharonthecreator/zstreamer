/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC source streaming node driver.
 *
 * This driver captures ADC samples using timer-triggered DMA and feeds
 * them into the zstreamer pipeline. Suitable for audio or signal capture
 * applications requiring precise sample rates.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

#if defined(CONFIG_ZSTREAMER_ADC_STM32)
#include "adc_src_stm32.h"
#endif

LOG_MODULE_REGISTER(adc_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#define DT_DRV_COMPAT zstreamer_adc_src

/*
 * ============================================================================
 * DATA STRUCTURES
 * ============================================================================
 */

struct adc_src_config {
  struct zstreamer_source_config common;

  /* ADC device (from first io-channel) */
  const struct device *adc_dev;

  /* ADC channels */
  uint8_t adc_channels[2];
  uint8_t num_channels;

  /* Timer for triggering - base address from devicetree */
  uintptr_t trigger_timer_addr;

  /* Sampling parameters */
  uint32_t sample_rate_hz;
  uint8_t resolution;
  uint16_t buffer_samples;
  uint16_t dma_buffer_samples;
};

struct adc_src_data {
  struct zstreamer_source_data common;

  /* Platform-specific state */
#if defined(CONFIG_ZSTREAMER_ADC_STM32)
  struct adc_src_stm32_data stm32;
#endif

  /* Streaming state */
  const struct device *dev;
  struct k_sem buffer_ready;
  const void *ready_buffer;
  size_t ready_samples;
  uint8_t ready_channels;
};

/*
 * ============================================================================
 * CALLBACK FROM PLATFORM LAYER
 * ============================================================================
 */

/**
 * Called by platform-specific code when a buffer half is ready.
 */
static void adc_buffer_ready_callback(void *user_data, const void *buffer,
                                      size_t num_samples, uint8_t channels) {
  struct adc_src_data *data = user_data;

  /* Store buffer info and signal the streaming thread */
  data->ready_buffer = buffer;
  data->ready_samples = num_samples;
  data->ready_channels = channels;

  k_sem_give(&data->buffer_ready);
}

/*
 * ============================================================================
 * ZSTREAMER SOURCE CALLBACKS
 * ============================================================================
 */

/**
 * Initialize the ADC source node.
 */
static int adc_src_init(const struct device *dev) {
  const struct adc_src_config *cfg = dev->config;
  struct adc_src_data *data = dev->data;
  int ret;

  data->dev = dev;

  /* Initialize semaphore for buffer synchronization */
  k_sem_init(&data->buffer_ready, 0, 1);

  /* Check that required devices are ready */
  if (!device_is_ready(cfg->adc_dev)) {
    LOG_ERR("ADC device not ready");
    return -ENODEV;
  }

  /* Timer is configured directly via register address, not device API */
  if (cfg->trigger_timer_addr == 0) {
    LOG_ERR("Trigger timer address not set");
    return -EINVAL;
  }

#if defined(CONFIG_ZSTREAMER_ADC_STM32)
  /* Configure STM32-specific ADC capture */
  struct adc_src_stm32_config stm32_cfg = {
      .adc_dev = cfg->adc_dev,
      .adc_channels = {cfg->adc_channels[0], cfg->adc_channels[1]},
      .num_channels = cfg->num_channels,
      .timer_addr = cfg->trigger_timer_addr,
      .sample_rate_hz = cfg->sample_rate_hz,
      .resolution = cfg->resolution,
      .buffer_samples = cfg->buffer_samples,
      .dma_buffer_samples = cfg->dma_buffer_samples,
      .callback = adc_buffer_ready_callback,
      .user_data = data,
  };

  ret = adc_src_stm32_init(&data->stm32, &stm32_cfg);
  if (ret != 0) {
    LOG_ERR("Failed to initialize STM32 ADC: %d", ret);
    return ret;
  }

  ret = adc_src_stm32_start(&data->stm32);
  if (ret != 0) {
    LOG_ERR("Failed to start STM32 ADC DMA: %d", ret);
    return ret;
  }
#else
  LOG_ERR("No platform-specific ADC implementation available");
  return -ENOTSUP;
#endif

  LOG_INF("ADC source %s initialized: %u Hz, %u-bit, %u ch", dev->name,
          cfg->sample_rate_hz, cfg->resolution, cfg->num_channels);

  return zstreamer_source_common_init(dev);
}

/**
 * Process function - called by streaming thread with a pre-allocated buffer.
 *
 * This waits for the DMA callback to signal buffer ready, then copies
 * the data into the provided streaming buffer.
 */
static int adc_src_process(const struct device *dev, struct net_buf *buf) {
  const struct adc_src_config *cfg = dev->config;
  struct adc_src_data *data = dev->data;
  int ret;

  /* Wait for buffer ready signal from DMA callback */
  ret = k_sem_take(&data->buffer_ready, K_MSEC(100));
  if (ret == -EAGAIN) {
    /* Timeout - no data ready yet */
    return -EAGAIN;
  }

  if (data->ready_buffer == NULL || data->ready_samples == 0) {
    return -EAGAIN;
  }

  /* Calculate data size */
  size_t sample_size = (cfg->resolution > 8) ? 2 : 1;
  size_t data_size = data->ready_samples * data->ready_channels * sample_size;

  /* Ensure we don't overflow the buffer */
  if (data_size > net_buf_tailroom(buf)) {
    LOG_WRN("Data size %zu exceeds buffer capacity %zu", data_size,
            net_buf_tailroom(buf));
    data_size = net_buf_tailroom(buf);
  }

  /* Copy data to streaming buffer */
  net_buf_add_mem(buf, data->ready_buffer, data_size);

  return 0;
}

/*
 * ============================================================================
 * DRIVER API
 * ============================================================================
 */

static const struct zstreamer_node_driver_api adc_src_api = {
    .process = adc_src_process,
};

/*
 * ============================================================================
 * DEVICE INSTANTIATION
 * ============================================================================
 */

/* Helper macro to get ADC channel from io-channels */
#define ADC_CHANNEL_GET(node_id, idx) DT_IO_CHANNELS_INPUT_BY_IDX(node_id, idx)

/* Helper macro to get ADC device from io-channels */
#define ADC_DEV_GET(node_id)                                                   \
  DEVICE_DT_GET(DT_IO_CHANNELS_CTLR_BY_IDX(node_id, 0))

/* Helper macro to count io-channels */
#define ADC_NUM_CHANNELS(node_id) MIN(DT_PROP_LEN(node_id, io_channels), 2)

#define ADC_SRC_DEFINE(inst)                                                   \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
  static struct adc_src_data adc_src_data_##inst = {                           \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                              \
      .ready_buffer = NULL,                                                    \
      .ready_samples = 0,                                                      \
      .ready_channels = 0,                                                     \
  };                                                                           \
  static const struct adc_src_config adc_src_config_##inst = {                 \
      .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                            \
      .adc_dev = ADC_DEV_GET(DT_DRV_INST(inst)),                               \
      .adc_channels =                                                          \
          {                                                                    \
              ADC_CHANNEL_GET(DT_DRV_INST(inst), 0),                           \
              COND_CODE_1(DT_INST_PROP_HAS_IDX(inst, io_channels, 1),          \
                          (ADC_CHANNEL_GET(DT_DRV_INST(inst), 1)), (0)),       \
          },                                                                   \
      .num_channels = ADC_NUM_CHANNELS(                                        \
          DT_DRV_INST(inst)), /* Timer base address from devicetree */         \
      .trigger_timer_addr = DT_REG_ADDR(DT_INST_PHANDLE(inst, trigger_timer)), \
      .sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                    \
      .resolution = DT_INST_PROP(inst, resolution),                            \
      .buffer_samples = DT_INST_PROP(inst, buffer_samples),                    \
      .dma_buffer_samples = DT_INST_PROP(inst, dma_buffer_samples),            \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, adc_src_init, NULL, &adc_src_data_##inst,        \
                        &adc_src_config_##inst, POST_KERNEL,                   \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &adc_src_api);

DT_INST_FOREACH_STATUS_OKAY(ADC_SRC_DEFINE)
