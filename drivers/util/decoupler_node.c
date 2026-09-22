/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stereo-channel decoupler node for zstreamer.
 *
 * Splits each left-right-interleaved input buffer into separate left and right
 * buffers.  Elements are copied as opaque element-size byte sequences, so
 * their signedness and byte order do not matter.
 */

#define DT_DRV_COMPAT zstreamer_decoupler_node

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

LOG_MODULE_REGISTER(decoupler_node, CONFIG_ZSTREAMER_LOG_LEVEL);

struct decoupler_node_config {
	struct zstreamer_node_config common;
	const struct device *const *left_children;
	size_t num_left_children;
	const struct device *const *right_children;
	size_t num_right_children;
	size_t element_size;
};

struct decoupler_node_data {
	struct zstreamer_node_data common;
};

static int decoupler_node_process(const struct device *dev, struct net_buf *buf)
{
	const struct decoupler_node_config *cfg = dev->config;
	const size_t frame_size = 2U * cfg->element_size;
	struct net_buf *left_buf;
	struct net_buf *right_buf;
	size_t channel_len;

	if ((buf->len % frame_size) != 0U) {
		LOG_ERR("[%s] buffer length %u is not a multiple of frame size %zu", dev->name,
			buf->len, frame_size);
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

	if (cfg->element_size == sizeof(uint16_t)) {
		const uint8_t *src = buf->data;
		uint8_t *left = left_buf->data;
		uint8_t *right = right_buf->data;

		for (size_t frame_offset = 0; frame_offset < buf->len;
		     frame_offset += sizeof(uint32_t)) {
			uint32_t stereo_pair;

			/* Fixed-size copies let the compiler emit an unaligned-safe load and
			 * two stores without changing the samples' byte representation. */
			memcpy(&stereo_pair, src, sizeof(stereo_pair));
			memcpy(left, &stereo_pair, sizeof(uint16_t));
			memcpy(right, (const uint8_t *)&stereo_pair + sizeof(uint16_t),
			       sizeof(uint16_t));

			src += sizeof(stereo_pair);
			left += sizeof(uint16_t);
			right += sizeof(uint16_t);
		}
	} else {
		for (size_t frame_offset = 0, output_offset = 0; frame_offset < buf->len;
		     frame_offset += frame_size, output_offset += cfg->element_size) {
			memcpy(left_buf->data + output_offset, buf->data + frame_offset,
			       cfg->element_size);
			memcpy(right_buf->data + output_offset,
			       buf->data + frame_offset + cfg->element_size, cfg->element_size);
		}
	}

	zstreamer_node_distribute(dev, left_buf, cfg->left_children, cfg->num_left_children);
	zstreamer_node_distribute(dev, right_buf, cfg->right_children, cfg->num_right_children);

	/* The channel buffers were distributed manually; ask the common node
	 * thread to consume the original interleaved buffer without forwarding it. */
	return -EAGAIN;
}

static int decoupler_node_init(const struct device *dev)
{
	const struct decoupler_node_config *cfg = dev->config;

	LOG_INF("decoupler_node %s: stereo element-size=%zu", dev->name, cfg->element_size);

	return zstreamer_node_common_init(dev);
}

static const struct zstreamer_node_driver_api decoupler_node_api = {
	.process = decoupler_node_process,
};

/* clang-format off */
#define DECOUPLER_CHILD_DEV_GET(node_id, prop, idx)                          \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))

#define DECOUPLER_LEFT_CHILDREN_DEFINE(inst)                                 \
	static const struct device *const decoupler_node_left_children_##inst[] = { \
		DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), left_children,          \
					 DECOUPLER_CHILD_DEV_GET, (, ))}

#define DECOUPLER_RIGHT_CHILDREN_DEFINE(inst)                                  \
	static const struct device *const decoupler_node_right_children_##inst[] = { \
		DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), right_children,          \
					 DECOUPLER_CHILD_DEV_GET, (, ))}
/* clang-format on */

#define DECOUPLER_NODE_DEFINE(inst)                                                                \
	BUILD_ASSERT(DT_INST_PROP(inst, element_size) > 0, "element-size must be > 0");            \
	BUILD_ASSERT(DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_count) > 2,                      \
		     "buffer-count must be greater than 2");                                       \
	ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                                                   \
	DECOUPLER_LEFT_CHILDREN_DEFINE(inst);                                                      \
	DECOUPLER_RIGHT_CHILDREN_DEFINE(inst);                                                     \
	static struct decoupler_node_data decoupler_node_data_##inst = {                           \
		.common = ZSTREAMER_NODE_DATA_INIT(inst),                                          \
	};                                                                                         \
	static const struct decoupler_node_config decoupler_node_config_##inst = {                 \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.left_children = decoupler_node_left_children_##inst,                              \
		.num_left_children = DT_INST_PROP_LEN(inst, left_children),                        \
		.right_children = decoupler_node_right_children_##inst,                            \
		.num_right_children = DT_INST_PROP_LEN(inst, right_children),                      \
		.element_size = DT_INST_PROP(inst, element_size),                                  \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, decoupler_node_init, NULL, &decoupler_node_data_##inst,        \
			      &decoupler_node_config_##inst, POST_KERNEL,                          \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &decoupler_node_api);

DT_INST_FOREACH_STATUS_OKAY(DECOUPLER_NODE_DEFINE)
