/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Looper processor node for zstreamer.
 *
 * Retransmits each incoming net_buf a configurable number of times.
 * Useful for reliability over lossy transports (e.g. LoRa) — repeat
 * the same packet multiple times without allocating extra memory.
 *
 * The gap between retransmissions is either fixed (delay-ms) or drawn
 * uniformly from [random-delay-min-ms, random-delay-max-ms] per gap —
 * jitter decorrelates repeats from a periodic interferer (and from
 * other transmitters repeating on the same schedule).  The two forms
 * are mutually exclusive, enforced at build time.  Internally a fixed
 * delay is just min == max.
 *
 * In process(): call zstreamer_node_distribute() manually
 * (retransmit_count - 1) times with net_buf_ref() to keep the buf
 * alive, then return 0 so the framework does the final distribute.
 */

#define DT_DRV_COMPAT zstreamer_looper_node

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

/* Only pull in (and link against) a random source when some instance
 * actually uses random delays. */
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(random_delay_min_ms)
#define LOOPER_NODE_USE_RANDOM 1
#include <zephyr/random/random.h>
#endif

LOG_MODULE_REGISTER(looper_node, CONFIG_ZSTREAMER_LOG_LEVEL);

struct looper_node_config {
	struct zstreamer_node_config common;
	uint32_t retransmit_count;
	uint32_t delay_min_ms;
	uint32_t delay_max_ms;
};

struct looper_node_data {
	struct zstreamer_node_data common;
};

static uint32_t looper_node_delay_ms(const struct looper_node_config *cfg)
{
	uint32_t delay = cfg->delay_min_ms;

#ifdef LOOPER_NODE_USE_RANDOM
	if (cfg->delay_max_ms > cfg->delay_min_ms) {
		delay += sys_rand32_get() % (cfg->delay_max_ms - cfg->delay_min_ms + 1);
	}
#endif

	return delay;
}

static int looper_node_process(const struct device *dev, struct net_buf *buf)
{
	const struct looper_node_config *cfg = dev->config;

	/* Send (retransmit_count - 1) extra copies manually. */
	for (uint32_t i = 1; i < cfg->retransmit_count; i++) {
		struct net_buf *ref = net_buf_ref(buf);
		ARG_UNUSED(ref);
		zstreamer_node_distribute(dev, buf, cfg->common.children, cfg->common.num_children);

		uint32_t delay = looper_node_delay_ms(cfg);

		if (delay > 0) {
			k_msleep(delay);
		}
	}

	/* Return 0 — framework does the final (Nth) distribute and unrefs. */
	return 0;
}

static int looper_node_init(const struct device *dev)
{
	const struct looper_node_config *cfg = dev->config;

	LOG_INF("looper_node %s: retransmit=%u delay=%u-%u ms", dev->name, cfg->retransmit_count,
		cfg->delay_min_ms, cfg->delay_max_ms);

	return zstreamer_node_common_init(dev);
}

static const struct zstreamer_node_driver_api looper_node_api = {
	.process = looper_node_process,
};

#define LOOPER_HAS_RANDOM(inst) DT_INST_NODE_HAS_PROP(inst, random_delay_min_ms)

#define LOOPER_NODE_DEFINE(inst)                                                                   \
	BUILD_ASSERT(!(DT_INST_NODE_HAS_PROP(inst, delay_ms) && LOOPER_HAS_RANDOM(inst)),          \
		     "configure either delay-ms or random-delay-*-ms, not both");                  \
	BUILD_ASSERT(LOOPER_HAS_RANDOM(inst) == DT_INST_NODE_HAS_PROP(inst, random_delay_max_ms),  \
		     "random-delay-min-ms and random-delay-max-ms go together");                   \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, random_delay_min_ms, 0) <=                              \
			     DT_INST_PROP_OR(inst, random_delay_max_ms, 0),                        \
		     "random-delay-min-ms must not exceed random-delay-max-ms");                   \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct looper_node_data looper_node_data_##inst = {                                 \
		.common = ZSTREAMER_NODE_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct looper_node_config looper_node_config_##inst = {                       \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.retransmit_count = DT_INST_PROP(inst, retransmit_count),                          \
		.delay_min_ms = COND_CODE_1(LOOPER_HAS_RANDOM(inst),                                 \
                      (DT_INST_PROP(inst, random_delay_min_ms)),               \
                      (DT_INST_PROP_OR(inst, delay_ms, 0))),                                \
			       .delay_max_ms = COND_CODE_1(LOOPER_HAS_RANDOM(inst),                                 \
                      (DT_INST_PROP(inst, random_delay_max_ms)),               \
                      (DT_INST_PROP_OR(inst, delay_ms, 0))),                 \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, looper_node_init, NULL, &looper_node_data_##inst,              \
			      &looper_node_config_##inst, POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &looper_node_api);

DT_INST_FOREACH_STATUS_OKAY(LOOPER_NODE_DEFINE)
