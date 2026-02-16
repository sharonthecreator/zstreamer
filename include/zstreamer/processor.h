/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for zstreamer processor nodes
 *
 * Processor nodes receive buffers, transform them via a process()
 * callback, and distribute the result to their children.
 */

#ifndef ZSTREAMER_PROCESSOR_H_
#define ZSTREAMER_PROCESSOR_H_

#include <zstreamer/node.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstreamer processor node API
 * @defgroup zstreamer_processor_interface zstreamer processor node API
 * @ingroup io_interfaces
 * @{
 */

struct zstreamer_processor_config {
  struct zstreamer_node_config common;
  const struct device *const *children;
  size_t num_children;
};

struct zstreamer_processor_data {
  struct zstreamer_node_data common;
};

/**
 * @brief Processor node driver API.
 *
 * @param open    Optional. Called at boot to set up hardware.
 * @param close   Optional. Called for cleanup.
 * @param process Required. Called for each received buffer. Return 0
 *                on success; non-zero causes the buffer to be dropped.
 */
__subsystem struct zstreamer_processor_driver_api {
  int (*open)(const struct device *dev);
  int (*close)(const struct device *dev);
  int (*process)(const struct device *dev, struct net_buf *buf);
};

/** @cond INTERNAL_HIDDEN */

extern int zstreamer_processor_common_init(const struct device *dev);

#define Z_ZSTREAMER_PROCESSOR_CONFIG_INIT(inst, node_id, _stack_size, _prio)   \
  .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(node_id, _stack_size, _prio)},       \
  .children = zstreamer_processor_children_##inst,                             \
  .num_children = Z_ZSTREAMER_NUM_CHILDREN(node_id)

#define Z_ZSTREAMER_PROCESSOR_DATA_INIT(inst, _stack)                          \
  {                                                                            \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(_stack),                            \
  }

#define Z_ZSTREAMER_PROCESSOR_INIT_WRAPPER_DEFINE(inst, _driver_init)          \
  static int zstreamer_processor_init_##inst(const struct device *dev) {       \
    int (*init_fn)(const struct device *) = _driver_init;                      \
    if (init_fn != NULL) {                                                     \
      int ret = init_fn(dev);                                                  \
      if (ret != 0) {                                                          \
        return ret;                                                            \
      }                                                                        \
    }                                                                          \
    return zstreamer_processor_common_init(dev);                               \
  }

/**
 * @brief Define a zstreamer processor node device from a DT node identifier.
 */
#define ZSTREAMER_PROCESSOR_DT_DEFINE(inst, node_id, init_fn, data_ptr,        \
                                       cfg_ptr, api_ptr)                        \
  Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_processor, inst, node_id);             \
  static K_THREAD_STACK_DEFINE(zstreamer_processor_stack_##inst,               \
                               DT_PROP(node_id, thread_stack_size));           \
  Z_ZSTREAMER_PROCESSOR_INIT_WRAPPER_DEFINE(inst, init_fn)                     \
  DEVICE_DT_DEFINE(node_id, zstreamer_processor_init_##inst, NULL, data_ptr,   \
                   cfg_ptr, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,   \
                   api_ptr)

#define ZSTREAMER_PROCESSOR_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr,   \
                                            api_ptr)                            \
  ZSTREAMER_PROCESSOR_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn, data_ptr,    \
                                cfg_ptr, api_ptr)

/** @endcond */

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_PROCESSOR_H_ */
