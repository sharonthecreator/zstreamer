/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for zstreamer sink nodes
 *
 * Sink nodes are terminal: they receive and consume buffers but never
 * distribute to children.  The sink DTS binding deliberately omits the
 * children property so that connecting downstream nodes is caught at
 * build time.
 */

#ifndef ZSTREAMER_SINK_H_
#define ZSTREAMER_SINK_H_

#include <zstreamer/node.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief zstreamer sink node API
 * @defgroup zstreamer_sink_interface zstreamer sink node API
 * @ingroup io_interfaces
 * @{
 */

struct zstreamer_sink_config {
  struct zstreamer_node_config common;
  /* No children — sinks are terminal. */
};

struct zstreamer_sink_data {
  struct zstreamer_node_data common;
};

/**
 * @brief Sink node driver API.
 *
 * @param open    Optional. Called at boot to set up hardware.
 * @param close   Optional. Called for cleanup.
 * @param process Required. Called for each received buffer. Return 0
 *                on success; non-zero is logged as an error.
 */
__subsystem struct zstreamer_sink_driver_api {
  int (*open)(const struct device *dev);
  int (*close)(const struct device *dev);
  int (*process)(const struct device *dev, struct net_buf *buf);
};

/* Sink nodes have no start/stop — they run automatically. */

/** @cond INTERNAL_HIDDEN */

extern int zstreamer_sink_common_init(const struct device *dev);

#define Z_ZSTREAMER_SINK_CONFIG_INIT(node_id, _stack_size, _prio)              \
  .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(node_id, _stack_size, _prio)}

#define Z_ZSTREAMER_SINK_DATA_INIT(inst, _stack)                               \
  {                                                                            \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(_stack),                            \
  }

#define Z_ZSTREAMER_SINK_INIT_WRAPPER_DEFINE(inst, _driver_init)               \
  static int zstreamer_sink_init_##inst(const struct device *dev) {            \
    int (*init_fn)(const struct device *) = _driver_init;                      \
    if (init_fn != NULL) {                                                     \
      int ret = init_fn(dev);                                                  \
      if (ret != 0) {                                                          \
        return ret;                                                            \
      }                                                                        \
    }                                                                          \
    return zstreamer_sink_common_init(dev);                                    \
  }

/**
 * @brief Define a zstreamer sink node device from a DT node identifier.
 *
 * No children array is generated — sinks are terminal.
 */
#define ZSTREAMER_SINK_DT_DEFINE(inst, node_id, init_fn, data_ptr, cfg_ptr,    \
                                  api_ptr)                                      \
  static K_THREAD_STACK_DEFINE(zstreamer_sink_stack_##inst,                    \
                               DT_PROP(node_id, thread_stack_size));           \
  Z_ZSTREAMER_SINK_INIT_WRAPPER_DEFINE(inst, init_fn)                          \
  DEVICE_DT_DEFINE(node_id, zstreamer_sink_init_##inst, NULL, data_ptr,        \
                   cfg_ptr, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,   \
                   api_ptr)

#define ZSTREAMER_SINK_DT_INST_DEFINE(inst, init_fn, data_ptr, cfg_ptr,        \
                                       api_ptr)                                 \
  ZSTREAMER_SINK_DT_DEFINE(inst, DT_DRV_INST(inst), init_fn, data_ptr,         \
                           cfg_ptr, api_ptr)

/** @endcond */

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_SINK_H_ */
