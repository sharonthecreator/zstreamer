/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(uart2uart, LOG_LEVEL_INF);

#define SRC_NODE  DT_NODELABEL(uart_source)
#define SINK_NODE DT_NODELABEL(uart_sinker)

int main(void)
{
	const struct device *src = DEVICE_DT_GET(SRC_NODE);
	const struct device *sink = DEVICE_DT_GET(SINK_NODE);
	int ret;

	if (!device_is_ready(src) || !device_is_ready(sink)) {
		LOG_ERR("streaming devices not ready");
		return -ENODEV;
	}

	/* Start sink first so it is ready to consume buffers. */
	ret = zstreamer_start(sink);
	if (ret) {
		LOG_ERR("failed to start sink: %d", ret);
		return ret;
	}

	ret = zstreamer_start(src);
	if (ret) {
		LOG_ERR("failed to start source: %d", ret);
		return ret;
	}

	LOG_INF("uart2uart pipeline running");
	return 0;
}
