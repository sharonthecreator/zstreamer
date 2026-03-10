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
  bool autostart;
};

struct zstreamer_source_data {
  struct zstreamer_node_data common;
  atomic_t running;
  struct k_sem run_sem;
  struct k_sem idle_sem;
};

/**
 * @brief Start a source node.
 *
 * Sets the running flag and signals the source thread to begin
 * generating.
 *
 * @param dev Source node device.
 * @return 0 on success, -EALREADY if already running.
 */
int zstreamer_source_start(const struct device *dev);

/**
 * @brief Stop a source node.
 *
 * Clears the running flag, waits for the source thread to become idle,
 * and drains pending buffers.
 *
 * @param dev Source node device.
 * @return 0 on success, -EALREADY if already stopped.
 */
int zstreamer_source_stop(const struct device *dev);

/**
 * @brief Common init for source nodes.
 *
 * Initialises source-specific semaphores, then calls
 * zstreamer_node_common_init().  Use this as the Zephyr init function
 * for source drivers instead of zstreamer_node_common_init().
 */
extern int zstreamer_source_common_init(const struct device *dev);

/**
 * @brief Thread entry for source nodes.
 *
 * Declared here so ZSTREAMER_SOURCE_CONFIG_INIT can reference it.
 */
extern void zstreamer_source_thread_entry(void *p1, void *p2, void *p3);

/**
 * @brief Source node config initializer.
 *
 * Produces a braced initializer value for struct zstreamer_source_config.
 * Requires ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier so that the children array symbol is visible.
 *
 * Usage: .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_SOURCE_CONFIG_INIT(inst)                                     \
  {                                                                            \
      .common = {Z_ZSTREAMER_NODE_BASE_CONFIG_INIT(                            \
          inst, zstreamer_source_children_##inst,                              \
          Z_ZSTREAMER_NUM_CHILDREN(inst), zstreamer_source_thread_entry,       \
          false)},                                                             \
      .autostart = DT_INST_PROP_OR(inst, autostart, false),                    \
  }

/**
 * @brief Source node data initializer.
 *
 * Produces a braced initializer value for struct zstreamer_source_data.
 * Requires ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier so that the stack symbol is visible.  Initializes the running
 * flag to 0 (stopped).  Zephyr runtime fields and semaphores are
 * initialized later by zstreamer_node_common_init().
 *
 * Usage: .common = ZSTREAMER_SOURCE_DATA_INIT(inst),
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_SOURCE_DATA_INIT(inst)                                       \
  {                                                                            \
      .common = {Z_ZSTREAMER_NODE_BASE_DATA_INIT(zstreamer_source, inst)},     \
      .running = ATOMIC_INIT(0),                                               \
  }

/**
 * @brief Pre-define the thread stack and children array for a source node.
 *
 * Emits the static symbols required by ZSTREAMER_SOURCE_CONFIG_INIT
 * and ZSTREAMER_SOURCE_DATA_INIT:
 *   - zstreamer_source_children_<inst>  (device-pointer array)
 *   - zstreamer_source_stack_<inst>     (thread stack)
 *
 * Must be called BEFORE defining the driver's data/config structs so
 * that these symbols are visible to their initialisers.
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst)                              \
  Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_source, inst);                         \
  static K_THREAD_STACK_DEFINE(zstreamer_source_stack_##inst,                  \
                               ZSTREAMER_THREAD_STACK_SIZE)

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_SOURCE_H_ */
