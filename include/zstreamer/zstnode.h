/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for zstnode drivers
 */

#ifndef ZSTREAMER_ZSTNODE_H_
#define ZSTREAMER_ZSTNODE_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
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
	struct k_thread thread;
	k_thread_stack_t *stack;
	atomic_t running;
	struct k_sem run_sem;
	struct k_sem idle_sem;
};

/**
 * @brief zstnode driver API structure.
 *
 * Source nodes implement generate(); non-source nodes implement process().
 * The framework thread handles buffer allocation and distribution.
 *
 * @param open      Optional. Called to set up hardware. For non-source nodes
 *                  this is called at boot. For source nodes it is called by
 *                  zstnode_start().
 * @param close     Optional. Called to tear down hardware. For source nodes
 *                  it is called by zstnode_stop().
 * @param generate  Source nodes only. Called with a pre-allocated buffer;
 *                  the driver fills it with data. Return 0 on success;
 *                  non-zero causes the buffer to be dropped.
 * @param process   Non-source nodes. Called for each received buffer.
 *                  Return 0 on success; non-zero causes the buffer to be
 *                  dropped.
 */
__subsystem struct zstnode_driver_api {
	int (*open)(const struct device *dev);
	int (*close)(const struct device *dev);
	int (*generate)(const struct device *dev, struct net_buf *buf);
	int (*process)(const struct device *dev, struct net_buf *buf);
};

/**
 * @brief Start a source node.
 *
 * Sets the running flag, calls the driver's open callback, and signals
 * the source thread to begin processing. Only valid for source nodes.
 *
 * @param dev Node device.
 * @return 0 on success, -EALREADY if already running, -ENOTSUP if
 *         not a source, or negative errno on failure.
 */
int zstnode_start(const struct device *dev);

/**
 * @brief Stop a source node.
 *
 * Clears the running flag, waits for the source thread to become idle,
 * drains pending buffers, and calls the driver's close callback.
 * Only valid for source nodes.
 *
 * @param dev Node device.
 * @return 0 on success, -EALREADY if already stopped, -ENOTSUP if
 *         not a source, or negative errno on failure.
 */
int zstnode_stop(const struct device *dev);

/**
 * @brief Allocate a buffer from the node's graph pool.
 *
 * @param dev     Node device.
 * @param timeout Allocation timeout.
 * @return Pointer to allocated net_buf, or NULL on timeout.
 */
struct net_buf *zstnode_alloc_buf(const struct device *dev,
				  k_timeout_t timeout);

/** @cond INTERNAL_HIDDEN */

/**
 * Common init function called from the device init wrapper.
 * Sets the dev back-pointer, initializes the fifo and semaphores,
 * calls open() for non-source nodes, and creates the thread.
 */
extern int zstnode_common_init(const struct device *dev);

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
#define Z_ZSTNODE_COMMON_CONFIG_INIT(inst, node_id, _stack_size, _prio)        \
	{                                                                      \
		.graph = DEVICE_DT_GET(DT_PARENT(node_id)),                    \
		.children = zstnode_children_##inst,                            \
		.num_children = Z_ZSTNODE_NUM_CHILDREN(node_id),               \
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
		int (*init_fn)(const struct device *) = _driver_init;          \
		if (init_fn != NULL) {                                         \
			int ret = init_fn(dev);                                \
			if (ret != 0) {                                        \
				return ret;                                    \
			}                                                      \
		}                                                              \
		return zstnode_common_init(dev);                               \
	}

/** @endcond */

/**
 * @brief Define a zstnode device from a devicetree node identifier.
 *
 * All nodes (source, sink, or generic) use this single macro.
 * A dedicated thread stack is always allocated.
 *
 * @param inst       Unique instance number (used for symbol naming).
 * @param node_id    Devicetree node identifier.
 * @param init_fn    Driver init function (or NULL).
 * @param data_ptr   Pointer to driver-specific data struct.
 * @param cfg_ptr    Pointer to driver-specific config struct.
 * @param api_ptr    Pointer to zstnode_driver_api struct.
 */
#define ZSTNODE_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr, api_ptr)  \
	Z_ZSTNODE_CHILDREN_DEFINE(inst, node_id);                              \
	static K_THREAD_STACK_DEFINE(                                          \
		zstnode_stack_##inst,                                           \
		DT_PROP(node_id, thread_stack_size));                          \
	Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, init_fn)                           \
	DEVICE_DT_DEFINE(node_id, zstnode_init_##inst, NULL,                   \
			 data_ptr, cfg_ptr, POST_KERNEL,                       \
			 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, api_ptr)

/**
 * @brief Instance-based node definition macro.
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

#endif /* ZSTREAMER_ZSTNODE_H_ */
