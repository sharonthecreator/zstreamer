/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(adc2fakesink, LOG_LEVEL_INF);

#define ADC_SRC_NODE  DT_NODELABEL(adc_source)
#define SINK_NODE     DT_NODELABEL(fake_sinker)

int main(void)
{
	const struct device *adc_src = DEVICE_DT_GET(ADC_SRC_NODE);
	const struct device *sink = DEVICE_DT_GET(SINK_NODE);
	int ret;

	LOG_INF("ADC to FakeSink sample");

	if (!device_is_ready(adc_src)) {
		LOG_ERR("ADC source device not ready");
		return -ENODEV;
	}

	if (!device_is_ready(sink)) {
		LOG_ERR("Sink device not ready");
		return -ENODEV;
	}

	/* Start sink first so it is ready to consume buffers */
	ret = zstreamer_start(sink);
	if (ret) {
		LOG_ERR("Failed to start sink: %d", ret);
		return ret;
	}

	ret = zstreamer_start(adc_src);
	if (ret) {
		LOG_ERR("Failed to start ADC source: %d", ret);
		return ret;
	}

	LOG_INF("ADC capture pipeline running");

	/* Let it run for a while, then stop */
	k_sleep(K_SECONDS(10));

	LOG_INF("Stopping pipeline");
	zstreamer_stop(adc_src);
	zstreamer_stop(sink);

	LOG_INF("Done");
	return 0;
}
