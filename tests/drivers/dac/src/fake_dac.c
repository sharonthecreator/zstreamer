/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal fake DAC driver for testing the dac_sink.
 * Records a history of written values for data integrity verification.
 */

#define DT_DRV_COMPAT zstreamer_fake_dac

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/sys/atomic.h>

#include "fake_dac.h"

struct fake_dac_data {
  atomic_t write_count;
  uint32_t last_value;
  uint32_t recorded[FAKE_DAC_MAX_RECORDED];
  uint32_t num_recorded;
};

static int fake_dac_channel_setup(const struct device *dev,
                                  const struct dac_channel_cfg *channel_cfg) {
  ARG_UNUSED(dev);
  ARG_UNUSED(channel_cfg);
  return 0;
}

static int fake_dac_write_value(const struct device *dev, uint8_t channel,
                                uint32_t value) {
  struct fake_dac_data *data = dev->data;

  ARG_UNUSED(channel);

  data->last_value = value;

  if (data->num_recorded < FAKE_DAC_MAX_RECORDED) {
    data->recorded[data->num_recorded] = value;
    data->num_recorded++;
  }

  atomic_inc(&data->write_count);

  return 0;
}

uint32_t fake_dac_get_write_count(const struct device *dev) {
  struct fake_dac_data *data = dev->data;
  return (uint32_t)atomic_get(&data->write_count);
}

uint32_t fake_dac_get_last_value(const struct device *dev) {
  struct fake_dac_data *data = dev->data;
  return data->last_value;
}

uint32_t fake_dac_get_recorded_count(const struct device *dev) {
  struct fake_dac_data *data = dev->data;
  return data->num_recorded;
}

uint32_t fake_dac_get_recorded_value(const struct device *dev, uint32_t idx) {
  struct fake_dac_data *data = dev->data;

  if (idx >= data->num_recorded) {
    return 0;
  }
  return data->recorded[idx];
}

void fake_dac_reset(const struct device *dev) {
  struct fake_dac_data *data = dev->data;
  atomic_set(&data->write_count, 0);
  data->last_value = 0;
  data->num_recorded = 0;
  memset(data->recorded, 0, sizeof(data->recorded));
}

static int fake_dac_init(const struct device *dev) {
  ARG_UNUSED(dev);
  return 0;
}

static DEVICE_API(dac, fake_dac_api) = {
    .channel_setup = fake_dac_channel_setup,
    .write_value = fake_dac_write_value,
};

#define FAKE_DAC_DEFINE(inst)                                                  \
  static struct fake_dac_data fake_dac_data_##inst;                            \
  DEVICE_DT_INST_DEFINE(inst, fake_dac_init, NULL, &fake_dac_data_##inst,      \
                        NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
                        &fake_dac_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_DAC_DEFINE)
