/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(sine2pwm, LOG_LEVEL_INF);

#define SRC_NODE DT_NODELABEL(sine_source)

int main(void) {
  const struct device *src = DEVICE_DT_GET(SRC_NODE);
  int ret;

  if (!device_is_ready(src)) {
    LOG_ERR("source device not ready");
    return -ENODEV;
  }

  ret = zstreamer_source_start(src);
  if (ret) {
    LOG_ERR("failed to start pipeline: %d", ret);
    return ret;
  }

  LOG_INF("sine2pwm pipeline running");
  return 0;
}
