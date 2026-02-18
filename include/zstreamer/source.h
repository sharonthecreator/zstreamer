/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for zstreamer source nodes
 *
 * Source nodes actively produce data by calling a driver-supplied
 * generate() callback in a loop.  They support start/stop lifecycle
 * control and distribute produced buffers to their children.
 */

#ifndef ZSTREAMER_SOURCE_H_
#define ZSTREAMER_SOURCE_H_

#include <zstreamer/node.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstreamer source node API
 * @defgroup zstreamer_source_interface zstreamer source node API
 * @ingroup io_interfaces
 * @{
 */

struct zstreamer_source_config {
  struct zstreamer_node_config common;
};

struct zstreamer_source_data {
  struct zstreamer_node_data common;
  atomic_t running;
  struct k_sem run_sem;
  struct k_sem idle_sem;
};

/**
 * @brief Source node driver API.
 *
 * @param open      Optional. Called by zstreamer_source_start() before
 *                  the generate loop begins.
 * @param close     Optional. Called by zstreamer_source_stop() after
 *                  the generate loop ends.
 * @param generate  Required. Called with a pre-allocated buffer; the
 *                  driver fills it with data. Return 0 on success;
 *                  non-zero causes the buffer to be dropped.
 */
__subsystem struct zstreamer_source_driver_api {
  int (*open)(const struct device *dev);
  int (*close)(const struct device *dev);
  int (*generate)(const struct device *dev, struct net_buf *buf);
};

/**
 * @brief Start a source node.
 *
 * Sets the running flag, calls the driver's open callback, and signals
 * the source thread to begin generating.
 *
 * @param dev Source node device.
 * @return 0 on success, -EALREADY if already running, or negative errno.
 */
int zstreamer_source_start(const struct device *dev);

/**
 * @brief Stop a source node.
 *
 * Clears the running flag, waits for the source thread to become idle,
 * drains pending buffers, and calls the driver's close callback.
 *
 * @param dev Source node device.
 * @return 0 on success, -EALREADY if already stopped, or negative errno.
 */
int zstreamer_source_stop(const struct device *dev);

/** @cond INTERNAL_HIDDEN */

extern int zstreamer_source_common_init(const struct device *dev);

#define Z_ZSTREAMER_SOURCE_CONFIG_INIT(inst, node_id, _stack_size, _prio)      \
  .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(node_id, _stack_size, _prio,         \
      zstreamer_source_children_##inst, Z_ZSTREAMER_NUM_CHILDREN(node_id))}

#define Z_ZSTREAMER_SOURCE_DATA_INIT(inst, _stack)                             \
  {                                                                            \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(_stack),                            \
  }

#define Z_ZSTREAMER_SOURCE_INIT_WRAPPER_DEFINE(inst, _driver_init)             \
  static int zstreamer_source_init_##inst(const struct device *dev) {          \
    int (*init_fn)(const struct device *) = _driver_init;                      \
    if (init_fn != NULL) {                                                     \
      int ret = init_fn(dev);                                                  \
      if (ret != 0) {                                                          \
        return ret;                                                            \
      }                                                                        \
    }                                                                          \
    return zstreamer_source_common_init(dev);                                  \
  }

/**
 * @brief Pre-define the thread stack and children array for a source node.
 *
 * Call this BEFORE defining the driver's data/config structs so the
 * stack and children symbols are visible to their initialisers.
 */
#define ZSTREAMER_SOURCE_DT_PRE_DEFINE(inst, node_id)                          \
  Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_source, inst, node_id);                \
  static K_THREAD_STACK_DEFINE(zstreamer_source_stack_##inst,                  \
                               DT_PROP(node_id, thread_stack_size))

#define ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst)                              \
  ZSTREAMER_SOURCE_DT_PRE_DEFINE(inst, DT_DRV_INST(inst))

/**
 * @brief Define a zstreamer source node device from a DT node identifier.
 *
 * The thread stack and children array must already have been emitted
 * with ZSTREAMER_SOURCE_DT_PRE_DEFINE / ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE.
 */
#define ZSTREAMER_SOURCE_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr,  \
                                    api_ptr)                                    \
  Z_ZSTREAMER_SOURCE_INIT_WRAPPER_DEFINE(inst, init_fn)                        \
  DEVICE_DT_DEFINE(node_id, zstreamer_source_init_##inst, NULL, data_ptr,      \
                   cfg_ptr, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,   \
                   api_ptr)

#define ZSTREAMER_SOURCE_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr,      \
                                         api_ptr)                               \
  ZSTREAMER_SOURCE_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn, data_ptr,       \
                             cfg_ptr, api_ptr)

/** @endcond */

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_SOURCE_H_ */
