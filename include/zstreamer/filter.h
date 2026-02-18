/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for zstreamer filter nodes
 *
 * Filter nodes receive buffers, run a boolean filter callback, and
 * distribute the buffer to either the normal children (filter returns
 * true) or the false-children (filter returns false).
 */

#ifndef ZSTREAMER_FILTER_H_
#define ZSTREAMER_FILTER_H_

#include <zstreamer/node.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstreamer filter node API
 * @defgroup zstreamer_filter_interface zstreamer filter node API
 * @ingroup io_interfaces
 * @{
 */

struct zstreamer_filter_config {
  struct zstreamer_node_config common;
  const struct device *const *false_children;
  size_t num_false_children;
};

struct zstreamer_filter_data {
  struct zstreamer_node_data common;
};

/**
 * @brief Filter node driver API.
 *
 * @param open   Optional. Called at boot to set up hardware.
 * @param close  Optional. Called for cleanup.
 * @param filter Required. Called for each received buffer. Return true
 *               to distribute to children, false to distribute to
 *               false-children.
 */
__subsystem struct zstreamer_filter_driver_api {
  int (*open)(const struct device *dev);
  int (*close)(const struct device *dev);
  bool (*filter)(const struct device *dev, struct net_buf *buf);
};

/** @cond INTERNAL_HIDDEN */

extern int zstreamer_filter_common_init(const struct device *dev);

#define Z_ZSTREAMER_FILTER_CONFIG_INIT(inst, node_id, _stack_size, _prio)      \
  .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(node_id, _stack_size, _prio,         \
      zstreamer_filter_children_##inst, Z_ZSTREAMER_NUM_CHILDREN(node_id))},   \
  .false_children = zstreamer_filter_false_children_##inst,                    \
  .num_false_children = Z_ZSTREAMER_NUM_FALSE_CHILDREN(node_id)

#define Z_ZSTREAMER_FILTER_DATA_INIT(inst, _stack)                             \
  {                                                                            \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(_stack),                            \
  }

#define Z_ZSTREAMER_FILTER_INIT_WRAPPER_DEFINE(inst, _driver_init)             \
  static int zstreamer_filter_init_##inst(const struct device *dev) {          \
    int (*init_fn)(const struct device *) = _driver_init;                      \
    if (init_fn != NULL) {                                                     \
      int ret = init_fn(dev);                                                  \
      if (ret != 0) {                                                          \
        return ret;                                                            \
      }                                                                        \
    }                                                                          \
    return zstreamer_filter_common_init(dev);                                  \
  }

/**
 * @brief Pre-define the thread stack, children, and false-children arrays
 *        for a filter node.
 *
 * Call this BEFORE defining the driver's data/config structs so the
 * stack, children, and false-children symbols are visible to their
 * initialisers.
 */
#define ZSTREAMER_FILTER_DT_PRE_DEFINE(inst, node_id)                          \
  Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_filter, inst, node_id);                \
  Z_ZSTREAMER_FALSE_CHILDREN_DEFINE(zstreamer_filter, inst, node_id);          \
  static K_THREAD_STACK_DEFINE(zstreamer_filter_stack_##inst,                  \
                               DT_PROP(node_id, thread_stack_size))

#define ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst)                              \
  ZSTREAMER_FILTER_DT_PRE_DEFINE(inst, DT_DRV_INST(inst))

/**
 * @brief Define a zstreamer filter node device from a DT node identifier.
 *
 * The thread stack, children, and false-children arrays must already
 * have been emitted with ZSTREAMER_FILTER_DT_PRE_DEFINE /
 * ZSTREAMER_FILTER_DT_INST_PRE_DEFINE.
 */
#define ZSTREAMER_FILTER_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr,  \
                                    api_ptr)                                    \
  Z_ZSTREAMER_FILTER_INIT_WRAPPER_DEFINE(inst, init_fn)                        \
  DEVICE_DT_DEFINE(node_id, zstreamer_filter_init_##inst, NULL, data_ptr,      \
                   cfg_ptr, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,   \
                   api_ptr)

#define ZSTREAMER_FILTER_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr,      \
                                         api_ptr)                               \
  ZSTREAMER_FILTER_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn, data_ptr,       \
                             cfg_ptr, api_ptr)

/** @endcond */

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_FILTER_H_ */
