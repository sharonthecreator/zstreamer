/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * 16-bit stereo deinterleaver node for zstreamer.
 *
 * Splits each L16, R16 interleaved input buffer into separate left and right
 * buffers. Sample signedness and byte order are preserved.
 */

#define DT_DRV_COMPAT zstreamer_stereo_deinterleaver_node

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(stereo_deinterleaver_node, CONFIG_ZSTREAMER_LOG_LEVEL);

#define STEREO_SAMPLE_SIZE sizeof(uint16_t)
#define STEREO_FRAME_SIZE  (2U * STEREO_SAMPLE_SIZE)

struct stereo_deinterleaver_config {
	struct zstreamer_node_config common;
	const struct device *const *left_children;
	size_t num_left_children;
	const struct device *const *right_children;
	size_t num_right_children;
};

struct stereo_deinterleaver_data {
	struct zstreamer_node_data common;
};

static int stereo_deinterleaver_process(const struct device *dev, struct net_buf *buf)
{
	const struct stereo_deinterleaver_config *cfg = dev->config;
	struct net_buf *left_buf;
	struct net_buf *right_buf;
	size_t channel_len;

	if ((buf->len % STEREO_FRAME_SIZE) != 0U) {
		LOG_ERR("[%s] buffer length %u is not a multiple of frame size %zu", dev->name,
			buf->len, STEREO_FRAME_SIZE);
		return -EAGAIN;
	}

	channel_len = buf->len / 2U;

	/* Allocate both outputs before publishing either one. */
	left_buf = zstreamer_node_alloc_buf(dev, K_NO_WAIT);
	if (left_buf == NULL) {
		LOG_ERR("[%s] failed to allocate left buffer", dev->name);
		return -EAGAIN;
	}

	right_buf = zstreamer_node_alloc_buf(dev, K_NO_WAIT);
	if (right_buf == NULL) {
		LOG_ERR("[%s] failed to allocate right buffer", dev->name);
		net_buf_unref(left_buf);
		return -EAGAIN;
	}

	net_buf_add(left_buf, channel_len);
	net_buf_add(right_buf, channel_len);

	for (size_t frame_offset = 0, output_offset = 0; frame_offset < buf->len;
	     frame_offset += STEREO_FRAME_SIZE, output_offset += STEREO_SAMPLE_SIZE) {
		memcpy(left_buf->data + output_offset, buf->data + frame_offset,
		       STEREO_SAMPLE_SIZE);
		memcpy(right_buf->data + output_offset,
		       buf->data + frame_offset + STEREO_SAMPLE_SIZE, STEREO_SAMPLE_SIZE);
	}

	zstreamer_node_distribute(dev, left_buf, cfg->left_children, cfg->num_left_children);
	zstreamer_node_distribute(dev, right_buf, cfg->right_children, cfg->num_right_children);

	/* The channel buffers were distributed manually; ask the common node
	 * thread to consume the original interleaved buffer without forwarding it. */
	return -EAGAIN;
}

static int stereo_deinterleaver_init(const struct device *dev)
{
	LOG_INF("stereo_deinterleaver_node %s: 16-bit samples", dev->name);

	return zstreamer_node_common_init(dev);
}

static const struct zstreamer_node_driver_api stereo_deinterleaver_api = {
	.process = stereo_deinterleaver_process,
};

/* clang-format off */
#define DEINTERLEAVER_CHILD_DEV_GET(node_id, prop, idx)                     \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))

#define DEINTERLEAVER_LEFT_CHILDREN_DEFINE(inst)                                  \
	static const struct device *const stereo_deinterleaver_left_children_##inst[] = { \
		DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), left_children,               \
					 DEINTERLEAVER_CHILD_DEV_GET, (, ))}

#define DEINTERLEAVER_RIGHT_CHILDREN_DEFINE(inst)                                  \
	static const struct device *const stereo_deinterleaver_right_children_##inst[] = { \
		DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), right_children,               \
					 DEINTERLEAVER_CHILD_DEV_GET, (, ))}
/* clang-format on */

#define STEREO_DEINTERLEAVER_DEFINE(inst)                                                          \
	BUILD_ASSERT(DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_count) > 2,                      \
		     "buffer-count must be greater than 2");                                       \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	DEINTERLEAVER_LEFT_CHILDREN_DEFINE(inst);                                                  \
	DEINTERLEAVER_RIGHT_CHILDREN_DEFINE(inst);                                                 \
	static struct stereo_deinterleaver_data stereo_deinterleaver_data_##inst = {               \
		.common = ZSTREAMER_NODE_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct stereo_deinterleaver_config stereo_deinterleaver_config_##inst = {     \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.left_children = stereo_deinterleaver_left_children_##inst,                        \
		.num_left_children = DT_INST_PROP_LEN(inst, left_children),                        \
		.right_children = stereo_deinterleaver_right_children_##inst,                      \
		.num_right_children = DT_INST_PROP_LEN(inst, right_children),                      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, stereo_deinterleaver_init, NULL,                               \
			      &stereo_deinterleaver_data_##inst,                                   \
			      &stereo_deinterleaver_config_##inst, POST_KERNEL,                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &stereo_deinterleaver_api);

DT_INST_FOREACH_STATUS_OKAY(STEREO_DEINTERLEAVER_DEFINE)
