/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(adc2dac, LOG_LEVEL_INF);

#define ADC_SRC_NODE DT_NODELABEL(adc_source)

int main(void)
{
	const struct device *adc_src = DEVICE_DT_GET(ADC_SRC_NODE);

	/* Pipeline starts automatically via DTS autostart property. */
	LOG_INF("ADC-to-DAC pipeline running (autostart)");

	/* Let it run for a while, then stop */
	k_sleep(K_SECONDS(10));

	LOG_INF("Stopping pipeline");
	zstreamer_source_stop(adc_src);

	LOG_INF("Done");
	return 0;
}
