/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstnode.h>

LOG_MODULE_REGISTER(spi2spi, LOG_LEVEL_INF);

#define SRC_NODE  DT_NODELABEL(spi_source)

int main(void)
{
	const struct device *src = DEVICE_DT_GET(SRC_NODE);
	int ret;

	if (!device_is_ready(src)) {
		LOG_ERR("streaming devices not ready");
		return -ENODEV;
	}

	ret = zstnode_start(src);
	if (ret) {
		LOG_ERR("failed to start pipeline: %d", ret);
		return ret;
	}

	LOG_INF("spi2spi pipeline running");
	return 0;
}
