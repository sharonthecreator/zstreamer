/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_lora_sink

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>

#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(lora_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct lora_sink_config {
	struct zstreamer_sink_config common;
	const struct device *lora_dev;
	struct lora_modem_config modem_cfg;
	struct gpio_dt_spec switch_gpio;
	uint32_t frequency_low;
	uint32_t frequency_high;
};

struct lora_sink_data {
	struct zstreamer_sink_data common;
	/* Carrier the radio is currently tuned to; 0 until the first successful
	 * lora_config().  Comparing it against the live pin level is what makes
	 * switch bounce a no-op -- only a settled, changed level reconfigures. */
	uint32_t applied_frequency;
};

/* Called before every send: the radio is idle between packets, so the switch
 * can be re-read and the band retuned without interrupting anything. */
static int lora_sink_configure(const struct device *dev)
{
	const struct lora_sink_config *cfg = dev->config;
	struct lora_sink_data *data = dev->data;
	uint32_t frequency = cfg->modem_cfg.frequency;

	if (data->applied_frequency == 0) {
		if (!device_is_ready(cfg->lora_dev)) {
			LOG_ERR("LoRa device not ready");
			return -ENODEV;
		}

		if (cfg->switch_gpio.port != NULL) {
			if (!gpio_is_ready_dt(&cfg->switch_gpio)) {
				LOG_ERR("Frequency switch GPIO not ready");
				return -ENODEV;
			}

			int ret = gpio_pin_configure_dt(&cfg->switch_gpio, GPIO_INPUT);

			if (ret < 0) {
				LOG_ERR("Failed to configure frequency switch GPIO: %d", ret);
				return ret;
			}
		}
	}

	if (cfg->switch_gpio.port != NULL) {
		int ret = gpio_pin_get_dt(&cfg->switch_gpio);

		if (ret < 0) {
			LOG_ERR("Failed to read frequency switch GPIO: %d", ret);
			return ret;
		}

		frequency = ret ? cfg->frequency_high : cfg->frequency_low;
	}

	if (frequency == data->applied_frequency) {
		return 0;
	}

	struct lora_modem_config modem_cfg = cfg->modem_cfg;

	modem_cfg.frequency = frequency;

	int ret = lora_config(cfg->lora_dev, &modem_cfg);

	if (ret < 0) {
		LOG_ERR("lora_config failed: %d", ret);
		return ret;
	}

	LOG_INF("LoRa sink %s: %u Hz, SF%u, BW%u, TX %d dBm", dev->name, modem_cfg.frequency,
		modem_cfg.datarate, modem_cfg.bandwidth, modem_cfg.tx_power);

	data->applied_frequency = frequency;
	return 0;
}

static int lora_sink_process(const struct device *dev, struct net_buf *buf)
{
	const struct lora_sink_config *cfg = dev->config;
	int ret;

	ret = lora_sink_configure(dev);
	if (ret < 0) {
		return ret;
	}

	if (buf->len == 0) {
		return 0;
	}

	ret = lora_send(cfg->lora_dev, buf->data, buf->len);

	if (ret < 0) {
		LOG_ERR("lora_send failed: %d", ret);
	}

	return ret;
}

static const struct zstreamer_node_driver_api lora_sink_api = {
	.process = lora_sink_process,
};

#define LORA_SINK_DEFINE(inst)                                                                     \
	ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct lora_sink_data lora_sink_data_##inst = {                                     \
		.common = ZSTREAMER_SINK_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct lora_sink_config lora_sink_config_##inst = {                           \
		.common = ZSTREAMER_SINK_CONFIG_INIT(inst),                                        \
		.lora_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, lora_device)),                     \
		.modem_cfg =                                                                       \
			{                                                                          \
				.frequency = DT_INST_PROP(inst, frequency),                        \
				.bandwidth = DT_INST_PROP(inst, bandwidth),                        \
				.datarate = DT_INST_PROP(inst, spreading_factor),                  \
				.coding_rate = DT_INST_PROP(inst, coding_rate),                    \
				.preamble_len = DT_INST_PROP(inst, preamble_length),               \
				.tx_power = DT_INST_PROP(inst, tx_power),                          \
				.tx = true,                                                        \
			},                                                                         \
		.switch_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, switch_gpios, {0}),                  \
		.frequency_low =                                                                   \
			DT_INST_PROP_OR(inst, frequency_low, DT_INST_PROP(inst, frequency)),       \
		.frequency_high =                                                                  \
			DT_INST_PROP_OR(inst, frequency_high, DT_INST_PROP(inst, frequency)),      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_sink_common_init, NULL, &lora_sink_data_##inst,      \
			      &lora_sink_config_##inst, POST_KERNEL,                               \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &lora_sink_api);

DT_INST_FOREACH_STATUS_OKAY(LORA_SINK_DEFINE)
