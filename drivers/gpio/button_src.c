/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Button source for zstreamer.
 *
 * Emits exactly one empty buffer per button press, routed by press
 * type: short presses go to children, long presses (held >=
 * long-press-ms, if long-press-ms > 0) go to long-press-children.
 *
 * Emit model: each process() call blocks until a press and emits one
 * buffer; the source framework loops and calls process() again.
 *
 * Optional periodic long-press mode (periodic-count / periodic-
 * interval-ms both set): a long press emits immediately, then keeps
 * re-emitting to long-press-children every periodic-interval-ms until
 * periodic-count total emissions have gone out. A short press cancels
 * an in-progress sequence; a long press while one is already running
 * restarts it from the top. The repeat is driven by a k_work_delayable
 * on the system workqueue rather than a blocking loop in this node's
 * own thread (cf. looper_node), specifically so this thread stays free
 * to take the press semaphore and react to the next press at any point
 * during the sequence. When both properties are absent, a long press
 * behaves exactly as if this feature didn't exist.
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
	uint32_t periodic_count;
	uint32_t periodic_interval_ms;
};

struct button_src_data {
	struct zstreamer_source_data common;
	struct gpio_callback cb_data;
	struct k_sem press_sem;
	const struct device *dev;
	struct k_work_delayable periodic_work;
	/* Recurring ticks left in the current periodic sequence (0 = none
	 * running). Only ever touched while periodic_work is provably idle
	 * (not scheduled, not running) — button_src_process() establishes
	 * that with k_work_cancel_delayable_sync() before reading or writing
	 * it, and the work handler is the sole writer while it runs. No lock
	 * needed as a result. */
	uint32_t periodic_remaining;
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

/* True if periodic mode is configured for this instance (both DT properties
 * set); false means a long press always emits exactly once. */
static bool button_src_has_periodic(const struct button_src_config *cfg)
{
	return cfg->periodic_count > 0;
}

/* System-workqueue tick for an active periodic sequence. Allocates its own
 * buffer per tick (rather than holding one net_buf ref across the whole
 * sequence) since a fresh buffer from the graph pool is simpler to reason
 * about than a multi-minute-lived ref, and the encode child downstream
 * overwrites the contents anyway. */
static void periodic_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct button_src_data *data = CONTAINER_OF(dwork, struct button_src_data, periodic_work);
	const struct button_src_config *cfg = data->dev->config;
	struct net_buf *buf = zstreamer_node_alloc_buf(data->dev, K_NO_WAIT);

	if (buf == NULL) {
		LOG_WRN("[%s] periodic tick: buf alloc failed, dropping this tick",
			data->dev->name);
	} else {
		zstreamer_node_distribute(data->dev, buf, cfg->long_press_children,
					  cfg->num_long_press_children);
	}

	data->periodic_remaining--;

	if (data->periodic_remaining > 0) {
		k_work_reschedule(&data->periodic_work, K_MSEC(cfg->periodic_interval_ms));
	} else {
		LOG_INF("[%s] periodic sequence complete", data->dev->name);
	}
}

static int button_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct button_src_config *cfg = dev->config;
	struct button_src_data *data = dev->data;
	struct k_work_sync work_sync;

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
		bool refresh = button_src_has_periodic(cfg) && data->periodic_remaining > 0;

		LOG_INF("[%s] long press%s", dev->name,
			refresh ? " (refreshing periodic sequence)" : "");

		if (button_src_has_periodic(cfg)) {
			/* Cancel-and-wait, not just cancel: guarantees the work handler
			 * isn't mid-run (about to decrement periodic_remaining/
			 * reschedule itself) before we overwrite that state below. The
			 * wait is bounded by one alloc+distribute, not the whole
			 * sequence. */
			k_work_cancel_delayable_sync(&data->periodic_work, &work_sync);
		}

		/* Route to the long-press children ourselves; the extra ref keeps buf
		 * alive for the framework's unref on the -EAGAIN return (distribute
		 * consumes one reference). This also serves as tick 1 of a periodic
		 * sequence. */
		struct net_buf *ref = net_buf_ref(buf);

		ARG_UNUSED(ref);
		zstreamer_node_distribute(dev, buf, cfg->long_press_children,
					  cfg->num_long_press_children);

		if (button_src_has_periodic(cfg) && cfg->periodic_count > 1) {
			data->periodic_remaining = cfg->periodic_count - 1;
			k_work_schedule(&data->periodic_work, K_MSEC(cfg->periodic_interval_ms));
		}

		return -EAGAIN;
	}

	/* Short press cancels any in-progress periodic sequence in addition to its
	 * normal distribute below. Same cancel-and-wait reasoning as above. */
	if (button_src_has_periodic(cfg) && data->periodic_remaining > 0) {
		k_work_cancel_delayable_sync(&data->periodic_work, &work_sync);
		data->periodic_remaining = 0;
		LOG_INF("[%s] short press cancels periodic sequence", dev->name);
	}

	/* Short press: framework distributes to children. */
	LOG_INF("[%s] short press", dev->name);
	return 0;
}

static int button_src_init(const struct device *dev)
{
	const struct button_src_config *cfg = dev->config;
	struct button_src_data *data = dev->data;
	int ret;

	data->dev = dev;

	k_sem_init(&data->press_sem, 0, 1);
	k_work_init_delayable(&data->periodic_work, periodic_work_handler);

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

#define BUTTON_SRC_HAS_PERIODIC_COUNT(inst)    DT_INST_NODE_HAS_PROP(inst, periodic_count)
#define BUTTON_SRC_HAS_PERIODIC_INTERVAL(inst) DT_INST_NODE_HAS_PROP(inst, periodic_interval_ms)

#define BUTTON_SRC_DEFINE(inst)                                                                    \
	BUILD_ASSERT(BUTTON_SRC_HAS_PERIODIC_COUNT(inst) ==                                        \
			     BUTTON_SRC_HAS_PERIODIC_INTERVAL(inst),                               \
		     "periodic-count and periodic-interval-ms go together");                       \
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
		.periodic_count = DT_INST_PROP_OR(inst, periodic_count, 0),                        \
		.periodic_interval_ms = DT_INST_PROP_OR(inst, periodic_interval_ms, 0),            \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, button_src_init, NULL, &button_src_data_##inst,                \
			      &button_src_config_##inst, POST_KERNEL,                              \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &button_src_api);

DT_INST_FOREACH_STATUS_OKAY(BUTTON_SRC_DEFINE)
