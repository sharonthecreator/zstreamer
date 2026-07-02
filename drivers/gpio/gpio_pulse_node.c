/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO pulse processor node for zstreamer.
 *
 * Sets a GPIO pin HIGH when a buffer arrives, waits a configurable
 * delay, passes the buffer downstream, then sets the pin LOW after
 * a configurable hold time.
 */

#define DT_DRV_COMPAT zstreamer_gpio_pulse_node

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(gpio_pulse_node, CONFIG_ZSTREAMER_LOG_LEVEL);

struct gpio_pulse_node_config {
	struct zstreamer_node_config common;
	struct gpio_dt_spec gpio;
	uint32_t hold_ms;
	uint32_t delay_ms;
};

struct gpio_pulse_node_data {
	struct zstreamer_node_data common;
	struct k_timer off_timer;
	const struct device *dev;
};

static void gpio_pulse_off_handler(struct k_timer *timer)
{
	struct gpio_pulse_node_data *data =
		CONTAINER_OF(timer, struct gpio_pulse_node_data, off_timer);
	const struct gpio_pulse_node_config *cfg = data->dev->config;

	gpio_pin_set_dt(&cfg->gpio, 0);
	LOG_DBG("pulse pin LOW");
}

static int gpio_pulse_node_process(const struct device *dev, struct net_buf *buf)
{
	const struct gpio_pulse_node_config *cfg = dev->config;
	struct gpio_pulse_node_data *data = dev->data;

	gpio_pin_set_dt(&cfg->gpio, 1);
	LOG_DBG("pulse pin HIGH (delay %u ms, hold %u ms)", cfg->delay_ms, cfg->hold_ms);

	/* Start the timer, though make it live at least the hold time
	 * after the delay itself. */
	k_timer_start(&data->off_timer, K_MSEC(cfg->hold_ms + cfg->delay_ms), K_NO_WAIT);

	/* Give the downstream peripheral time to be ready before forwarding
	 * the buffer. */
	k_msleep(cfg->delay_ms);

	return 0;
}

static int gpio_pulse_node_init(const struct device *dev)
{
	const struct gpio_pulse_node_config *cfg = dev->config;
	struct gpio_pulse_node_data *data = dev->data;
	int ret;

	data->dev = dev;

	if (!gpio_is_ready_dt(&cfg->gpio)) {
		LOG_ERR("gpio_pulse_node: GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->gpio, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("gpio_pulse_node: failed to configure GPIO: %d", ret);
		return ret;
	}

	k_timer_init(&data->off_timer, gpio_pulse_off_handler, NULL);

	LOG_INF("gpio_pulse_node %s: hold=%u ms, delay=%u ms", dev->name, cfg->hold_ms,
		cfg->delay_ms);

	return zstreamer_node_common_init(dev);
}

static const struct zstreamer_node_driver_api gpio_pulse_node_api = {
	.process = gpio_pulse_node_process,
};

#define GPIO_PULSE_NODE_DEFINE(inst)                                                               \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct gpio_pulse_node_data gpio_pulse_node_data_##inst = {                         \
		.common = ZSTREAMER_NODE_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct gpio_pulse_node_config gpio_pulse_node_config_##inst = {               \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.gpio = GPIO_DT_SPEC_INST_GET(inst, gpios),                                        \
		.hold_ms = DT_INST_PROP(inst, hold_ms),                                            \
		.delay_ms = DT_INST_PROP(inst, delay_ms),                                          \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, gpio_pulse_node_init, NULL, &gpio_pulse_node_data_##inst,      \
			      &gpio_pulse_node_config_##inst, POST_KERNEL,                         \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &gpio_pulse_node_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_PULSE_NODE_DEFINE)
