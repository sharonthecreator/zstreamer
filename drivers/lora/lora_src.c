/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_lora_src

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(lora_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#define MAX_LORA_RECV_SIZE 255

/* How often RX is broken off to re-read the frequency switch.  lora_recv()
 * cannot be aborted and lora_config() is -EBUSY while the radio is in RX, so
 * a bounded timeout is the only way to notice the switch moved.  Each expiry
 * kills any packet still in the air, costing roughly (airtime + restart)/T of
 * the traffic -- ~4% at SF5/BW10.42kHz, which sender-side retransmits absorb.
 * Only used when switch-gpios is wired; otherwise RX stays continuous. */
#define BAND_RECHECK_TIMEOUT K_SECONDS(2)

struct lora_src_config {
	struct zstreamer_source_config common;
	const struct device *lora_dev;
	struct lora_modem_config modem_cfg;
	struct gpio_dt_spec switch_gpio;
	uint32_t frequency_low;
	uint32_t frequency_high;
};

struct lora_src_data {
	struct zstreamer_source_data common;
	/* Carrier the radio is currently tuned to; 0 until the first successful
	 * lora_config().  Comparing it against the live pin level is what makes
	 * switch bounce a no-op -- only a settled, changed level reconfigures. */
	uint32_t applied_frequency;
	uint8_t rx_buf[MAX_LORA_RECV_SIZE];
};

static int lora_src_configure(const struct device *dev)
{
	const struct lora_src_config *cfg = dev->config;
	struct lora_src_data *data = dev->data;
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

	LOG_INF("LoRa source %s: %u Hz, SF%u, BW%u", dev->name, modem_cfg.frequency,
		modem_cfg.datarate, modem_cfg.bandwidth);

	data->applied_frequency = frequency;
	return 0;
}

static int lora_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct lora_src_config *cfg = dev->config;
	int16_t rssi;
	int8_t snr;
	int ret;

	ret = lora_src_configure(dev);
	if (ret < 0) {
		return ret;
	}

	struct lora_src_data *data = dev->data;
	uint8_t max_len = MIN(net_buf_tailroom(buf), MAX_LORA_RECV_SIZE);

	k_timeout_t timeout = cfg->switch_gpio.port != NULL ? BAND_RECHECK_TIMEOUT : K_FOREVER;

	int len = lora_recv(cfg->lora_dev, data->rx_buf, max_len, timeout, &rssi, &snr);

	if (len == -EAGAIN) {
		/* Recheck window expired; the source loop calls us straight back
		 * and lora_src_configure() re-reads the switch. */
		return -EAGAIN;
	}

	if (len < 0) {
		LOG_ERR("lora_recv failed: %d", len);
		return len;
	}

	if (len == 0) {
		LOG_DBG("RX zero-length packet, dropping");
		return -EAGAIN;
	}

	memcpy(net_buf_add(buf, len), data->rx_buf, len);
	LOG_DBG("RX %d bytes, RSSI %d dBm, SNR %d dB", len, rssi, snr);

	return 0;
}

static const struct zstreamer_node_driver_api lora_src_api = {
	.process = lora_src_process,
};

#define LORA_SRC_DEFINE(inst)                                                                      \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	static struct lora_src_data lora_src_data_##inst = {                                       \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct lora_src_config lora_src_config_##inst = {                             \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.lora_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, lora_device)),                     \
		.modem_cfg =                                                                       \
			{                                                                          \
				.frequency = DT_INST_PROP(inst, frequency),                        \
				.bandwidth = DT_INST_PROP(inst, bandwidth),                        \
				.datarate = DT_INST_PROP(inst, spreading_factor),                  \
				.coding_rate = DT_INST_PROP(inst, coding_rate),                    \
				.preamble_len = DT_INST_PROP(inst, preamble_length),               \
				.tx_power = DT_INST_PROP(inst, tx_power),                          \
				.tx = false,                                                       \
			},                                                                         \
		.switch_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, switch_gpios, {0}),                  \
		.frequency_low =                                                                   \
			DT_INST_PROP_OR(inst, frequency_low, DT_INST_PROP(inst, frequency)),       \
		.frequency_high =                                                                  \
			DT_INST_PROP_OR(inst, frequency_high, DT_INST_PROP(inst, frequency)),      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, zstreamer_source_common_init, NULL, &lora_src_data_##inst,     \
			      &lora_src_config_##inst, POST_KERNEL,                                \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &lora_src_api);

DT_INST_FOREACH_STATUS_OKAY(LORA_SRC_DEFINE)
