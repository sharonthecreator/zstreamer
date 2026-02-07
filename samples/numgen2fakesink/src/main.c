/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(numgen2fakesink, LOG_LEVEL_INF);

#define SRC_NODE DT_NODELABEL(numgen_source)

int main(void)
{
	const struct device *src = DEVICE_DT_GET(SRC_NODE);
	int ret;

	if (!device_is_ready(src)) {
		LOG_ERR("source device not ready");
		return -ENODEV;
	}

	/* Starting the source auto-starts all downstream children. */
	ret = zstreamer_start(src);
	if (ret) {
		LOG_ERR("failed to start pipeline: %d", ret);
		return ret;
	}

	LOG_INF("numgen2fakesink pipeline running");
	return 0;
}
