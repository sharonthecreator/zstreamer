/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the zstreamer framework
 */

#ifndef ZSTREAMER_ZSTREAMER_H_
#define ZSTREAMER_ZSTREAMER_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstreamer framework API
 * @defgroup zstreamer_api zstreamer framework
 * @{
 */

/** Graph device configuration (populated by DT macros). */
struct zstreamer_graph_config {
	struct net_buf_pool *pool;
};

/** Graph device runtime data. */
struct zstreamer_graph_data {
	atomic_t started;
};

/**
 * @brief Start a streaming node and all its downstream children.
 *
 * Recursively starts children (depth-first) before starting the node
 * itself, so sinks are ready before sources begin producing data.
 * For source/sink nodes this creates the dedicated thread.
 * Calls the driver's optional open callback.
 *
 * Children that are already running (-EALREADY) are silently skipped.
 *
 * @param dev Node device.
 * @return 0 on success, negative errno on failure.
 */
int zstreamer_start(const struct device *dev);

/**
 * @brief Stop a streaming node and all its downstream children.
 *
 * Stops this node first (clears running flag, joins thread, drains fifo,
 * calls close callback), then recursively stops children.
 * Children that are already stopped (-EALREADY) are silently skipped.
 *
 * @param dev Node device.
 * @return 0 on success, negative errno on failure.
 */
int zstreamer_stop(const struct device *dev);

/**
 * @brief Submit a buffer to all children of a node.
 *
 * For a single child: net_buf_ref + k_fifo_put.
 * For multiple children: first child gets net_buf_ref, additional
 * children get net_buf_clone from the graph pool.
 * After distribution the caller's reference is released.
 * For generic children a k_work is submitted to the system workqueue.
 *
 * @param dev  Node device that produced the buffer.
 * @param buf  Buffer to submit. Caller must not use buf after this call.
 * @return 0 on success, negative errno on failure.
 */
int zstreamer_submit_buffer(const struct device *dev, struct net_buf *buf);

/**
 * @brief Allocate a buffer from the node's graph pool.
 *
 * @param dev     Node device.
 * @param timeout Allocation timeout.
 * @return Pointer to allocated net_buf, or NULL on timeout.
 */
struct net_buf *zstreamer_alloc_buf(const struct device *dev,
				    k_timeout_t timeout);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_ZSTREAMER_H_ */
