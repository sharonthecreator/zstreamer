/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(adc2fakesink, LOG_LEVEL_INF);

#define ADC_SRC_NODE DT_NODELABEL(adc_source)

int main(void) {
  const struct device *adc_src = DEVICE_DT_GET(ADC_SRC_NODE);
  int ret;

  LOG_INF("ADC to FakeSink sample");

  if (!device_is_ready(adc_src)) {
    LOG_ERR("ADC source device not ready");
    return -ENODEV;
  }

  ret = zstreamer_node_start(adc_src);
  if (ret) {
    LOG_ERR("Failed to start pipeline: %d", ret);
    return ret;
  }

  LOG_INF("ADC capture pipeline running");

  /* Let it run for a while, then stop */
  k_sleep(K_SECONDS(10));

  LOG_INF("Stopping pipeline");
  zstreamer_node_stop(adc_src);

  LOG_INF("Done");
  return 0;
}
