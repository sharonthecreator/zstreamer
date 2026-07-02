/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Button source for zstreamer.
 *
 * Short press: emits exactly one empty buffer.
 * Long press (held >= long-press-ms, if long-press-ms > 0): "repeat mode" —
 * emits one empty buffer every repeat-interval-ms for repeat-duration-ms,
 * then returns to idle.
 *
 * Emit model: each process() call blocks/sleeps and returns 0 to emit exactly
 * one buffer; the source framework loops and calls process() again. State
 * lives in button_src_data so it survives across calls.
 */

#define DT_DRV_COMPAT zstreamer_button_src

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(button_src, CONFIG_ZSTREAMER_LOG_LEVEL);

/* Hold-detection poll step. Small enough to feel responsive on release,
 * coarse enough to double as debounce. */
#define BUTTON_SRC_POLL_MS 50

struct button_src_config {
	struct zstreamer_source_config common;
	struct gpio_dt_spec button;
	uint32_t long_press_ms;
	uint32_t repeat_interval_ms;
	uint32_t repeat_duration_ms;
};

struct button_src_data {
	struct zstreamer_source_data common;
	struct gpio_callback cb_data;
	struct k_sem press_sem;
	const struct device *dev;
	/* Repeat-mode state, carried across process() calls. */
	bool repeat_active;
	int64_t repeat_deadline; /* k_uptime_get() value at which repeat mode ends */
};

static void button_pressed_cb(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct button_src_data *data = CONTAINER_OF(cb, struct button_src_data, cb_data);

	k_sem_give(&data->press_sem);
}

/*
 * Returns true if the button is still held continuously through long_press_ms,
 * false if it was released first. Also serves as debounce: a sub-step glitch
 * release ends the hold and is reported as a short press.
 */
static bool button_held_for_long_press(const struct button_src_config *cfg)
{
	/* k_uptime_get() is int64_t ms; elapsed comparison is monotonic and never
	 * wraps over the relevant horizons here. */
	int64_t deadline = k_uptime_get() + cfg->long_press_ms;

	while (k_uptime_get() < deadline) {
		if (gpio_pin_get_dt(&cfg->button) != 1) {
			return false; /* released before threshold -> short press */
		}
		k_msleep(BUTTON_SRC_POLL_MS);
	}

	/* Confirm still held at the threshold (final debounce sample). */
	return gpio_pin_get_dt(&cfg->button) == 1;
}

static int button_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct button_src_config *cfg = dev->config;
	struct button_src_data *data = dev->data;

	/* Already in repeat mode: pace subsequent emits and auto-stop on
	 * deadline. */
	if (data->repeat_active) {
		if (k_uptime_get() >= data->repeat_deadline) {
			data->repeat_active = false;
			/* Drain presses queued during repeat mode so they don't fire a
			 * spurious emit the instant we go idle. */
			k_sem_reset(&data->press_sem);
			/* fall through to idle/blocking path below */
		} else {
			k_msleep(cfg->repeat_interval_ms);
			return 0; /* one emit per interval */
		}
	}

	/* Idle: block until a press wakes us. */
	k_sem_take(&data->press_sem, K_FOREVER);

	/* long-press-ms == 0 disables repeat mode: every press is short. */
	bool long_press = cfg->long_press_ms > 0 && button_held_for_long_press(cfg);

	/* Drain any presses queued by bounce or a release+re-press during the hold
	 * poll, so one physical press resolves to exactly one logical event.
	 * Without this a stale give makes the framework re-enter process()
	 * immediately and emit a spurious second buffer. */
	k_sem_reset(&data->press_sem);

	if (long_press) {
		/* Enter repeat mode. Emit the first buffer immediately; subsequent
		 * emits are paced by the repeat_active branch above. Deadline bounds
		 * the mode to repeat_duration_ms regardless of how long the button is
		 * held. */
		data->repeat_active = true;
		data->repeat_deadline = k_uptime_get() + cfg->repeat_duration_ms;
		LOG_INF("[%s] repeat mode: emit every %u ms for %u ms", dev->name,
			cfg->repeat_interval_ms, cfg->repeat_duration_ms);
		return 0;
	}

	/* Short press: exactly one emit. */
	return 0;
}

static int button_src_init(const struct device *dev)
{
	const struct button_src_config *cfg = dev->config;
	struct button_src_data *data = dev->data;
	int ret;

	data->dev = dev;

	k_sem_init(&data->press_sem, 0, 1);

	if (!gpio_is_ready_dt(&cfg->button)) {
		LOG_ERR("Button GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure button GPIO: %d", ret);
		return ret;
	}

	/* Edge-to-active only wakes process() on press; hold/release is then read
	 * by polling the level in button_held_for_long_press(). */
	ret = gpio_pin_interrupt_configure_dt(&cfg->button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure button interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&data->cb_data, button_pressed_cb, BIT(cfg->button.pin));

	ret = gpio_add_callback(cfg->button.port, &data->cb_data);
	if (ret < 0) {
		LOG_ERR("Failed to add button callback: %d", ret);
		return ret;
	}

	return zstreamer_source_common_init(dev);
}

static const struct zstreamer_node_driver_api button_src_api = {
	.process = button_src_process,
};

#define BUTTON_SRC_DEFINE(inst)                                                                    \
	/* repeat_interval_ms is passed to k_msleep(), which takes int32_t; a                      \
	 * value above INT32_MAX would wrap negative and turn the paced emit into                  \
	 * a tight loop. Catch misconfiguration at build time. */                                  \
	BUILD_ASSERT(DT_INST_PROP(inst, repeat_interval_ms) <= INT32_MAX,                          \
		     "repeat-interval-ms must fit in int32_t (k_msleep)");                         \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	static struct button_src_data button_src_data_##inst = {                                   \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct button_src_config button_src_config_##inst = {                         \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.button = GPIO_DT_SPEC_INST_GET(inst, gpios),                                      \
		.long_press_ms = DT_INST_PROP(inst, long_press_ms),                                \
		.repeat_interval_ms = DT_INST_PROP(inst, repeat_interval_ms),                      \
		.repeat_duration_ms = DT_INST_PROP(inst, repeat_duration_ms),                      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, button_src_init, NULL, &button_src_data_##inst,                \
			      &button_src_config_##inst, POST_KERNEL,                              \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &button_src_api);

DT_INST_FOREACH_STATUS_OKAY(BUTTON_SRC_DEFINE)
