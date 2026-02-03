/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for zstnode drivers
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_ZSTNODE_H_
#define ZEPHYR_INCLUDE_DRIVERS_ZSTNODE_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net/buf.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstnode driver API
 * @defgroup zstnode_interface zstnode driver API
 * @ingroup io_interfaces
 * @{
 */

/** Node type enumeration. */
enum zstnode_type {
	ZSTNODE_TYPE_SOURCE,
	ZSTNODE_TYPE_SINK,
	ZSTNODE_TYPE_GENERIC,
};

/**
 * @brief Common configuration shared by all zstnode devices.
 *
 * Driver-specific config structs must embed this as the first member
 * named "common".
 */
struct zstnode_common_config {
	const struct device *graph;
	const struct device * const *children;
	size_t num_children;
	enum zstnode_type type;
	size_t thread_stack_size;
	int thread_priority;
};

/**
 * @brief Common runtime data shared by all zstnode devices.
 *
 * Driver-specific data structs must embed this as the first member
 * named "common".
 */
struct zstnode_common_data {
	const struct device *dev;
	struct k_fifo fifo;
	struct k_work work;
	struct k_thread thread;
	k_thread_stack_t *stack;
	atomic_t running;
};

/**
 * @brief zstnode driver API structure.
 *
 * @param start  Optional. Called when the node is started.
 * @param stop   Optional. Called when the node is stopped.
 * @param run    Source nodes only. Called in a loop from the source thread.
 *               Return 0 to continue, non-zero to stop.
 * @param process Sink/generic nodes. Called for each received buffer.
 *               Return 0 on success, non-zero on error.
 */
__subsystem struct zstnode_driver_api {
	int (*start)(const struct device *dev);
	int (*stop)(const struct device *dev);
	int (*run)(const struct device *dev);
	int (*process)(const struct device *dev, struct net_buf *buf);
};

/**
 * @brief Work handler for generic (workqueue-based) nodes.
 *
 * Defined in zstreamer_node.c. Drains the node fifo, calling
 * api->process for each buffer.
 */
extern void zstnode_generic_work_handler(struct k_work *work);

/** @cond INTERNAL_HIDDEN */

/**
 * Common init function called from the device init wrapper.
 * Sets the dev back-pointer, initializes the fifo, and for generic
 * nodes initializes the k_work.
 */
static inline int zstnode_common_init(const struct device *dev)
{
	struct zstnode_common_data *data = (struct zstnode_common_data *)dev->data;
	const struct zstnode_common_config *cfg =
		(const struct zstnode_common_config *)dev->config;

	data->dev = dev;
	k_fifo_init(&data->fifo);

	if (cfg->type == ZSTNODE_TYPE_GENERIC) {
		k_work_init(&data->work, zstnode_generic_work_handler);
	}

	return 0;
}

/*
 * Helper: generate children array from DT phandles.
 * Expands to an empty array if the node has no children property.
 */
#define Z_ZSTNODE_CHILD_DEV_GET(node_id, prop, idx) \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))

#define Z_ZSTNODE_CHILDREN_DEFINE(inst, node_id)                               \
	static const struct device * const                                     \
		zstnode_children_##inst[] = {                                   \
		COND_CODE_1(DT_NODE_HAS_PROP(node_id, children),              \
			(DT_FOREACH_PROP_ELEM_SEP(node_id, children,          \
				Z_ZSTNODE_CHILD_DEV_GET, (,))),                \
			())                                                    \
	}

#define Z_ZSTNODE_NUM_CHILDREN(node_id)                                        \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, children),                      \
		(DT_PROP_LEN(node_id, children)), (0))

/*
 * Common config initializer.
 */
#define Z_ZSTNODE_COMMON_CONFIG_INIT(inst, node_id, _type, _stack_size, _prio) \
	{                                                                      \
		.graph = DEVICE_DT_GET(DT_PARENT(node_id)),                    \
		.children = zstnode_children_##inst,                            \
		.num_children = Z_ZSTNODE_NUM_CHILDREN(node_id),               \
		.type = _type,                                                 \
		.thread_stack_size = _stack_size,                               \
		.thread_priority = _prio,                                      \
	}

/*
 * Common data initializer.
 */
#define Z_ZSTNODE_COMMON_DATA_INIT(inst, _stack)                               \
	{                                                                      \
		.stack = _stack,                                               \
	}

/*
 * Init wrapper: calls driver init (if provided), then common init.
 */
#define Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, _driver_init)                      \
	static int zstnode_init_##inst(const struct device *dev)               \
	{                                                                      \
		int ret = 0;                                                   \
		if (_driver_init != NULL) {                                    \
			ret = _driver_init(dev);                               \
			if (ret != 0) {                                        \
				return ret;                                    \
			}                                                      \
		}                                                              \
		return zstnode_common_init(dev);                               \
	}

/** @endcond */

/**
 * @brief Define a source node device from a devicetree node identifier.
 *
 * @param inst       Unique instance number (used for symbol naming).
 * @param node_id    Devicetree node identifier.
 * @param init_fn    Driver init function (or NULL).
 * @param data_ptr   Pointer to driver-specific data struct.
 * @param cfg_ptr    Pointer to driver-specific config struct.
 * @param api_ptr    Pointer to zstnode_driver_api struct.
 */
#define ZSTNODE_SRC_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr,       \
			      api_ptr)                                         \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, node_id);                              \
	static K_THREAD_STACK_DEFINE(                                          \
		zstnode_stack_##inst,                                           \
		DT_PROP(node_id, thread_stack_size));                          \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, init_fn)                           \
	DEVICE_DT_DEFINE(node_id, zstnode_init_##inst, NULL,                   \
			 data_ptr, cfg_ptr, POST_KERNEL,                       \
			 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, api_ptr)

/**
 * @brief Define a sink node device from a devicetree node identifier.
 *
 * Same as ZSTNODE_SRC_DT_DEFINE but for sink nodes.
 */
#define ZSTNODE_SINK_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr,      \
			       api_ptr)                                        \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, node_id);                              \
	static K_THREAD_STACK_DEFINE(                                          \
		zstnode_stack_##inst,                                           \
		DT_PROP(node_id, thread_stack_size));                          \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, init_fn)                           \
	DEVICE_DT_DEFINE(node_id, zstnode_init_##inst, NULL,                   \
			 data_ptr, cfg_ptr, POST_KERNEL,                       \
			 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, api_ptr)

/**
 * @brief Define a generic (workqueue-based) node device from a devicetree
 *        node identifier.
 *
 * Generic nodes have no dedicated thread; they process buffers on the
 * system workqueue.
 */
#define ZSTNODE_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr, api_ptr)  \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, node_id);                              \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, init_fn)                           \
	DEVICE_DT_DEFINE(node_id, zstnode_init_##inst, NULL,                   \
			 data_ptr, cfg_ptr, POST_KERNEL,                       \
			 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, api_ptr)

/*
 * DT_INST variants for use inside DT_INST_FOREACH_STATUS_OKAY.
 */

/**
 * @brief Instance-based source node definition macro.
 */
#define ZSTNODE_SRC_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr, api_ptr)  \
	ZSTNODE_SRC_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn,               \
			      data_ptr, cfg_ptr, api_ptr)

/**
 * @brief Instance-based sink node definition macro.
 */
#define ZSTNODE_SINK_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr, api_ptr) \
	ZSTNODE_SINK_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn,              \
			       data_ptr, cfg_ptr, api_ptr)

/**
 * @brief Instance-based generic node definition macro.
 */
#define ZSTNODE_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr, api_ptr)      \
	ZSTNODE_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn,                   \
			  data_ptr, cfg_ptr, api_ptr)

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_ZSTNODE_H_ */
