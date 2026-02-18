/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Base types and helpers shared by all zstreamer node types
 *
 * This header provides the common base configuration, runtime data,
 * and utility functions that all node types (source, sink, filter)
 * build upon.  It is also directly usable for through-nodes (nodes
 * that receive a buffer, optionally transform it, and forward to
 * children).  Type-specific headers (source.h, sink.h, filter.h)
 * extend it with additional semantics.
 */

#ifndef ZSTREAMER_NODE_H_
#define ZSTREAMER_NODE_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstreamer node base API
 * @defgroup zstreamer_node_interface zstreamer node base API
 * @ingroup io_interfaces
 * @{
 */

/**
 * @brief Base configuration shared by every zstreamer node device.
 *
 * Type-specific config structs embed this as the first member named
 * "common" so that generic helpers can cast any node's config pointer
 * to this type.
 */
struct zstreamer_node_config {
  const struct device *graph;
  size_t thread_stack_size;
  int thread_priority;
  bool readonly;
  const struct device *const *children;
  size_t num_children;
};

/**
 * @brief Base runtime data shared by every zstreamer node device.
 *
 * Type-specific data structs embed this as the first member named
 * "common" so that generic helpers can cast any node's data pointer
 * to this type.
 */
struct zstreamer_node_data {
  const struct device *dev;
  struct k_fifo fifo;
  struct k_thread thread;
  k_thread_stack_t *stack;
};

/**
 * @brief Through-node driver API.
 *
 * Through-nodes receive buffers, optionally transform them via a
 * process() callback, and distribute the result to their children.
 * This is the direct-use API for node.h — no separate "processor"
 * type is needed.
 *
 * @param open    Optional. Called at boot to set up hardware.
 * @param close   Optional. Called for cleanup.
 * @param process Required. Called for each received buffer. Return 0
 *                on success; non-zero causes the buffer to be dropped.
 */
__subsystem struct zstreamer_node_driver_api {
  int (*open)(const struct device *dev);
  int (*close)(const struct device *dev);
  int (*process)(const struct device *dev, struct net_buf *buf);
};

/**
 * @brief Allocate a buffer from the node's graph pool.
 *
 * @param dev     Any zstreamer node device.
 * @param timeout Allocation timeout.
 * @return Pointer to allocated net_buf, or NULL on timeout.
 */
struct net_buf *zstreamer_node_alloc_buf(const struct device *dev,
                                         k_timeout_t timeout);

/** @cond INTERNAL_HIDDEN */

/**
 * Base init: sets dev back-pointer, initializes the fifo, and creates
 * the node thread with the given entry point.
 */
extern int zstreamer_node_base_init(const struct device *dev,
                                     k_thread_entry_t entry);

/**
 * Common init for through-nodes: calls open(), then starts the node
 * thread with the through-node thread entry.
 */
extern int zstreamer_node_common_init(const struct device *dev);

/**
 * Distribute a buffer to an explicit list of child devices, honouring
 * the copy-on-write / readonly optimisation.
 */
extern void zstreamer_node_distribute(const struct device *dev,
                                       struct net_buf *buf,
                                       const struct device *const *children,
                                       size_t num_children);

/**
 * Drain (unref) all pending buffers from a fifo.
 */
extern void zstreamer_node_drain_fifo(struct k_fifo *fifo);

/*
 * Base config initializer.
 */
#define Z_ZSTREAMER_NODE_CONFIG_INIT(node_id, _stack_size, _prio,              \
                                     _children, _num_children)                  \
  .graph = DEVICE_DT_GET(DT_PARENT(node_id)),                                  \
  .thread_stack_size = _stack_size, .thread_priority = _prio,                   \
  .children = _children, .num_children = _num_children

/*
 * Base data initializer.
 */
#define Z_ZSTREAMER_NODE_DATA_INIT(_stack)                                     \
  {                                                                            \
      .stack = _stack,                                                         \
  }

/*
 * Helper: get a device pointer from a phandle-array element.
 */
#define Z_ZSTREAMER_NODE_CHILD_DEV_GET(node_id, prop, idx)                     \
  DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))

/*
 * Helper: define a children device-pointer array from DT phandles.
 */
#define Z_ZSTREAMER_CHILDREN_DEFINE(prefix, inst, node_id)                     \
  static const struct device *const prefix##_children_##inst[] = {             \
      COND_CODE_1(                                                             \
          DT_NODE_HAS_PROP(node_id, children),                                 \
          (DT_FOREACH_PROP_ELEM_SEP(node_id, children,                         \
                                    Z_ZSTREAMER_NODE_CHILD_DEV_GET, (, ))),    \
          ())}

#define Z_ZSTREAMER_NUM_CHILDREN(node_id)                                      \
  COND_CODE_1(DT_NODE_HAS_PROP(node_id, children),                             \
              (DT_PROP_LEN(node_id, children)), (0))

/*
 * Helper: define a false-children device-pointer array from DT phandles.
 */
#define Z_ZSTREAMER_FALSE_CHILDREN_DEFINE(prefix, inst, node_id)               \
  static const struct device *const prefix##_false_children_##inst[] = {       \
      COND_CODE_1(                                                             \
          DT_NODE_HAS_PROP(node_id, false_children),                           \
          (DT_FOREACH_PROP_ELEM_SEP(node_id, false_children,                   \
                                    Z_ZSTREAMER_NODE_CHILD_DEV_GET, (, ))),    \
          ())}

#define Z_ZSTREAMER_NUM_FALSE_CHILDREN(node_id)                                \
  COND_CODE_1(DT_NODE_HAS_PROP(node_id, false_children),                       \
              (DT_PROP_LEN(node_id, false_children)), (0))

#define Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, _driver_init)               \
  static int zstreamer_node_init_##inst(const struct device *dev) {            \
    int (*init_fn)(const struct device *) = _driver_init;                      \
    if (init_fn != NULL) {                                                     \
      int ret = init_fn(dev);                                                  \
      if (ret != 0) {                                                          \
        return ret;                                                            \
      }                                                                        \
    }                                                                          \
    return zstreamer_node_common_init(dev);                                    \
  }

/**
 * @brief Pre-define the thread stack and children array for a through-node.
 *
 * Call this BEFORE defining the driver's data/config structs so the
 * stack and children symbols are visible to their initialisers.
 */
#define ZSTREAMER_NODE_DT_PRE_DEFINE(inst, node_id)                            \
  Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_node, inst, node_id);                  \
  static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                    \
                               DT_PROP(node_id, thread_stack_size))

#define ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst)                                \
  ZSTREAMER_NODE_DT_PRE_DEFINE(inst, DT_DRV_INST(inst))

/**
 * @brief Define a zstreamer through-node device from a DT node identifier.
 *
 * The thread stack and children array must already have been emitted
 * with ZSTREAMER_NODE_DT_PRE_DEFINE / ZSTREAMER_NODE_DT_INST_PRE_DEFINE.
 */
#define ZSTREAMER_NODE_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr,    \
                                  api_ptr)                                      \
  Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, init_fn)                          \
  DEVICE_DT_DEFINE(node_id, zstreamer_node_init_##inst, NULL, data_ptr,        \
                   cfg_ptr, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,   \
                   api_ptr)

#define ZSTREAMER_NODE_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr,        \
                                       api_ptr)                                 \
  ZSTREAMER_NODE_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn, data_ptr,         \
                           cfg_ptr, api_ptr)

/** @endcond */

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_NODE_H_ */
