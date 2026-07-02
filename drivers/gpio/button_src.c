/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Button source for zstreamer.
 *
 * Emits exactly one empty buffer per button press, routed by press
 * type: short presses go to children, long presses (held >=
 * long-press-ms, if long-press-ms > 0) go to long-press-children.
 * Periodic/repeated behavior is composed downstream (e.g. a
 * looper_node on the long-press path), not built in.
 *
 * Emit model: each process() call blocks until a press and emits one
 * buffer; the source framework loops and calls process() again.
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
	const struct device *const *long_press_children;
	size_t num_long_press_children;
};

struct button_src_data {
	struct zstreamer_source_data common;
	struct gpio_callback cb_data;
	struct k_sem press_sem;
	const struct device *dev;
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

	/* Idle: block until a press wakes us. */
	k_sem_take(&data->press_sem, K_FOREVER);

	/* long-press-ms == 0 disables long-press detection: every press is
	 * short. */
	bool long_press = cfg->long_press_ms > 0 && button_held_for_long_press(cfg);

	/* Drain any presses queued by bounce or a release+re-press during the hold
	 * poll, so one physical press resolves to exactly one logical event.
	 * Without this a stale give makes the framework re-enter process()
	 * immediately and emit a spurious second buffer. */
	k_sem_reset(&data->press_sem);

	if (long_press) {
		/* Route to the long-press children ourselves; the extra ref keeps buf
		 * alive for the framework's unref on the -EAGAIN return (distribute
		 * consumes one reference). */
		LOG_INF("[%s] long press", dev->name);
		struct net_buf *ref = net_buf_ref(buf);

		ARG_UNUSED(ref);
		zstreamer_node_distribute(dev, buf, cfg->long_press_children,
					  cfg->num_long_press_children);
		return -EAGAIN;
	}

	/* Short press: framework distributes to children. */
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

#define BUTTON_SRC_LP_CHILD_GET(node_id, prop, idx)                                                \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))

/* clang-format off */
#define BUTTON_SRC_LP_CHILDREN_DEFINE(inst)                                    \
  static const struct device *const button_src_lp_children_##inst[] =         \
      {COND_CODE_1(                                                           \
          DT_NODE_HAS_PROP(DT_DRV_INST(inst), long_press_children),           \
          (DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), long_press_children,   \
                                    BUTTON_SRC_LP_CHILD_GET, (, ))),          \
          ())}

#define BUTTON_SRC_NUM_LP_CHILDREN(inst)                                       \
  COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), long_press_children),       \
              (DT_PROP_LEN(DT_DRV_INST(inst), long_press_children)), (0))
/* clang-format on */

#define BUTTON_SRC_DEFINE(inst)                                                                    \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	BUTTON_SRC_LP_CHILDREN_DEFINE(inst);                                                       \
	static struct button_src_data button_src_data_##inst = {                                   \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct button_src_config button_src_config_##inst = {                         \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.button = GPIO_DT_SPEC_INST_GET(inst, gpios),                                      \
		.long_press_ms = DT_INST_PROP(inst, long_press_ms),                                \
		.long_press_children = button_src_lp_children_##inst,                              \
		.num_long_press_children = BUTTON_SRC_NUM_LP_CHILDREN(inst),                       \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, button_src_init, NULL, &button_src_data_##inst,                \
			      &button_src_config_##inst, POST_KERNEL,                              \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &button_src_api);

DT_INST_FOREACH_STATUS_OKAY(BUTTON_SRC_DEFINE)
