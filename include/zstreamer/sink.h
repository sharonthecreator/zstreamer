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

/* Sink nodes have no start/stop — they run automatically. */

/**
 * @brief Common init for sink nodes.
 *
 * Calls zstreamer_node_common_init().  Use this as the Zephyr init
 * function for sink drivers instead of zstreamer_node_common_init()
 * for consistency with zstreamer_source_common_init().
 */
extern int zstreamer_sink_common_init(const struct device *dev);

/**
 * @brief Thread entry for sink nodes.
 *
 * Declared here so ZSTREAMER_SINK_CONFIG_INIT can reference it.
 */
extern void zstreamer_sink_thread_entry(void *p1, void *p2, void *p3);

/**
 * @brief Sink node config initializer.
 *
 * Produces a braced initializer value for struct zstreamer_sink_config.
 * Requires ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier.  Sets children to NULL and num_children to 0 since sinks
 * are terminal nodes with no downstream.
 *
 * Usage: .common = ZSTREAMER_SINK_CONFIG_INIT(inst),
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_SINK_CONFIG_INIT(inst)                                       \
  {                                                                            \
    .common = {                                                                \
      Z_ZSTREAMER_NODE_BASE_CONFIG_INIT(DT_DRV_INST(inst),                     \
                                        DT_INST_PROP(inst, thread_stack_size), \
                                        DT_INST_PROP(inst, thread_priority),   \
                                        NULL, 0, zstreamer_sink_thread_entry)  \
    }                                                                          \
  }

/**
 * @brief Sink node data initializer.
 *
 * Produces a braced initializer value for struct zstreamer_sink_data.
 * Requires ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier so that the stack symbol is visible.  Zephyr runtime fields
 * are initialized later by zstreamer_node_common_init().
 *
 * Usage: .common = ZSTREAMER_SINK_DATA_INIT(inst),
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_SINK_DATA_INIT(inst)                                         \
  {                                                                            \
      .common = {Z_ZSTREAMER_NODE_BASE_DATA_INIT(zstreamer_sink, inst)},       \
  }

/**
 * @brief Pre-define the thread stack for a sink node instance.
 *
 * Emits the static symbol required by ZSTREAMER_SINK_DATA_INIT:
 *   - zstreamer_sink_stack_<inst>  (thread stack)
 *
 * Sinks have no children array.  Must be called BEFORE defining the
 * driver's data/config structs so the stack symbol is visible.
 *
 * @param inst     Devicetree instance number.
 * @param node_id  Devicetree node identifier.
 */
#define ZSTREAMER_SINK_DT_PRE_DEFINE(inst, node_id)                            \
  static K_THREAD_STACK_DEFINE(zstreamer_sink_stack_##inst,                    \
                               DT_PROP(node_id, thread_stack_size))

/**
 * @brief Convenience wrapper for ZSTREAMER_SINK_DT_PRE_DEFINE using
 *        DT_DRV_INST.
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst)                                \
  ZSTREAMER_SINK_DT_PRE_DEFINE(inst, DT_DRV_INST(inst))

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_SINK_H_ */
