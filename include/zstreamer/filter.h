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
 * @brief Common init for filter nodes.
 *
 * Calls zstreamer_node_common_init().  Use this as the Zephyr init
 * function for filter drivers instead of zstreamer_node_common_init()
 * for consistency with zstreamer_source_common_init().
 */
extern int zstreamer_filter_common_init(const struct device *dev);

/**
 * @brief Thread entry for filter nodes.
 *
 * Declared here so ZSTREAMER_FILTER_CONFIG_INIT can reference it.
 */
extern void zstreamer_filter_thread_entry(void *p1, void *p2, void *p3);

/**
 * @brief Filter node config initializer.
 *
 * Produces a braced initializer value for struct zstreamer_filter_config.
 * Requires ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier so that the children and false-children array symbols are visible.
 * Initializes both the normal children (routed on process() returning 1)
 * and the false-children (routed on process() returning 0).
 *
 * Usage: .common = ZSTREAMER_FILTER_CONFIG_INIT(inst, true),
 *
 * @param inst      Devicetree instance number.
 * @param _readonly true if process() never modifies the buffer contents.
 */
#define ZSTREAMER_FILTER_CONFIG_INIT(inst, _readonly)                                              \
	{.common = {Z_ZSTREAMER_NODE_BASE_CONFIG_INIT(inst, zstreamer_filter_children_##inst,      \
						      Z_ZSTREAMER_NUM_CHILDREN(inst),              \
						      zstreamer_filter_thread_entry, _readonly)},  \
	 .false_children = zstreamer_filter_false_children_##inst,                                 \
	 .num_false_children = Z_ZSTREAMER_NUM_FALSE_CHILDREN(inst)}

/**
 * @brief Filter node data initializer.
 *
 * Produces a braced initializer value for struct zstreamer_filter_data.
 * Requires ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier so that the stack symbol is visible.  Zephyr runtime fields
 * are initialized later by zstreamer_node_common_init().
 *
 * Usage: .common = ZSTREAMER_FILTER_DATA_INIT(inst),
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_FILTER_DATA_INIT(inst)                                                           \
	{                                                                                          \
		.common = {Z_ZSTREAMER_NODE_BASE_DATA_INIT(zstreamer_filter, inst)},               \
	}

/**
 * @brief Pre-define the thread stack, children, and false-children arrays
 *        for a filter node.
 *
 * Emits the static symbols required by ZSTREAMER_FILTER_CONFIG_INIT
 * and ZSTREAMER_FILTER_DATA_INIT:
 *   - zstreamer_filter_children_<inst>        (device-pointer array)
 *   - zstreamer_filter_false_children_<inst>  (device-pointer array)
 *   - zstreamer_filter_stack_<inst>           (thread stack)
 *
 * Must be called BEFORE defining the driver's data/config structs so
 * that these symbols are visible to their initialisers.
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst)                                                  \
	Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_filter, inst);                                       \
	Z_ZSTREAMER_FALSE_CHILDREN_DEFINE(zstreamer_filter, inst);                                 \
	static K_THREAD_STACK_DEFINE(zstreamer_filter_stack_##inst, ZSTREAMER_THREAD_STACK_SIZE)

/**
 * @}
 */

/** @cond INTERNAL_HIDDEN */

/* clang-format off */
#define Z_ZSTREAMER_FALSE_CHILDREN_DEFINE(prefix, inst)                        \
  static const struct device *const prefix##_false_children_##inst[] = {       \
      COND_CODE_1(                                                             \
          DT_NODE_HAS_PROP(DT_DRV_INST(inst), false_children),                 \
          (DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), false_children,         \
                                    Z_ZSTREAMER_NODE_CHILD_DEV_GET, (, ))),    \
          ())}

#define Z_ZSTREAMER_NUM_FALSE_CHILDREN(inst)                                   \
  COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), false_children),             \
              (DT_PROP_LEN(DT_DRV_INST(inst), false_children)), (0))
/* clang-format on */

/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_FILTER_H_ */
