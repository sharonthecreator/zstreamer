/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_pwm_sink

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(pwm_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct pwm_sink_config {
  struct zstreamer_sink_config common;
  struct pwm_dt_spec pwm;
  uint8_t min_value;
  uint8_t max_value;
};

struct pwm_sink_data {
  struct zstreamer_sink_data common;
};

static int pwm_sink_process(const struct device *dev, struct net_buf *buf) {
  const struct pwm_sink_config *cfg = dev->config;
  uint32_t sum = 0;
  uint32_t pulse;

  if (buf->len == 0) {
    return 0;
  }

  for (size_t i = 0; i < buf->len; i++) {
    sum += buf->data[i];
  }

  uint8_t mean = sum / buf->len;

  if (mean <= cfg->min_value) {
    pulse = 0;
  } else if (mean >= cfg->max_value) {
    pulse = cfg->pwm.period;
  } else {
    pulse = (uint32_t)(mean - cfg->min_value) * cfg->pwm.period /
            (cfg->max_value - cfg->min_value);
  }

  int ret = pwm_set_pulse_dt(&cfg->pwm, pulse);

  if (ret < 0) {
    LOG_ERR("pwm_set_pulse_dt failed: %d", ret);
  }

  return ret;
}

static const struct zstreamer_node_driver_api pwm_sink_api = {
    .process = pwm_sink_process,
};

#define PWM_SINK_DEFINE(inst)                                                  \
  BUILD_ASSERT(DT_INST_PROP(inst, max_value) > DT_INST_PROP(inst, min_value),  \
               "pwm-sink: max-value must be greater than min-value");          \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                     \
  static struct pwm_sink_data pwm_sink_data_##inst = {                         \
      .common = ZSTREAMER_SINK_DATA_INIT(inst),                                \
  };                                                                           \
  static const struct pwm_sink_config pwm_sink_config_##inst = {               \
      .common = ZSTREAMER_SINK_CONFIG_INIT(inst),                              \
      .pwm = PWM_DT_SPEC_GET(DT_DRV_INST(inst)),                               \
      .min_value = DT_INST_PROP(inst, min_value),                              \
      .max_value = DT_INST_PROP(inst, max_value),                              \
  };                                                                           \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_sink_common_init, NULL,                \
                        &pwm_sink_data_##inst, &pwm_sink_config_##inst,        \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &pwm_sink_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_SINK_DEFINE)
