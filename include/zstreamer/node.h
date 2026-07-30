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
 * @brief Fixed thread stack size for all zstreamer node threads.
 *
 * Not expected to change.  If a driver needs more memory, allocate
 * it in the driver's data struct, not on the stack.
 */
#define ZSTREAMER_THREAD_STACK_SIZE 2048

/**
 * @brief Buffer type carried in every graph buffer's user_data.
 *
 * Ordinary data buffers are ZSTREAMER_BUF_DATA (0, the alloc default —
 * net_buf zeroes user_data on allocation).  Event buffers are
 * zero-length in-band control markers that travel through the same
 * fifos as data, so they stay ordered relative to the data around
 * them.  Sources emit EVENT_START when a run begins and EVENT_STOP
 * when it ends; the framework forwards events downstream and never
 * passes them to process().
 */
enum zstreamer_buf_type {
  ZSTREAMER_BUF_DATA = 0,
  ZSTREAMER_BUF_EVENT_START,
  ZSTREAMER_BUF_EVENT_STOP,
};

/**
 * @brief Per-buffer metadata stored in net_buf user_data.
 *
 * The graph pool reserves sizeof(struct zstreamer_buf_meta) bytes of
 * user_data on every buffer.  net_buf_clone() copies user_data, so
 * the type survives copy-on-write distribution.
 */
struct zstreamer_buf_meta {
  uint8_t type; /**< enum zstreamer_buf_type */
};

/** @brief Get the buffer type of a graph buffer. */
static inline enum zstreamer_buf_type
zstreamer_buf_type_get(const struct net_buf *buf) {
  const struct zstreamer_buf_meta *meta = net_buf_user_data(buf);

  if (buf->user_data_size < sizeof(*meta)) {
    /* Buffer from a foreign pool without metadata: treat as data. */
    return ZSTREAMER_BUF_DATA;
  }
  return (enum zstreamer_buf_type)meta->type;
}

/** @brief Set the buffer type of a graph buffer. */
static inline void zstreamer_buf_type_set(struct net_buf *buf,
                                          enum zstreamer_buf_type type) {
  struct zstreamer_buf_meta *meta = net_buf_user_data(buf);

  meta->type = (uint8_t)type;
}

/** @brief True if the buffer is an in-band event marker. */
static inline bool zstreamer_buf_is_event(const struct net_buf *buf) {
  return zstreamer_buf_type_get(buf) != ZSTREAMER_BUF_DATA;
}

/**
 * @brief Base configuration shared by every zstreamer node device.
 *
 * Type-specific config structs embed this as the first member named
 * "common" so that generic helpers can cast any node's config pointer
 * to this type.
 */
struct zstreamer_node_config {
  const struct device *graph;
  k_thread_entry_t thread_entry;
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
 * @brief Driver API shared by every zstreamer node type.
 *
 * All type-specific API structs embed this as the first member named
 * "common" so that generic helpers can cast any node's api pointer to
 * this type.  Driver setup belongs in the Zephyr init function; the
 * API struct carries only the runtime processing callback.
 *
 * @param process Required. Called for each received buffer.
 *                Return 0 on success, negative errno on error.
 *                Filter convention: 0 = false (route to false_children),
 *                1 = true (route to children), <0 = error.
 *                If the error is -EAGAIN, the buffer is unrefed silently.
 * @param handle_event Optional. Called for each received event buffer
 *                (events never reach process()).  Return 0 to let the
 *                framework forward the event downstream (filters
 *                forward to both child sets; sinks unref).  Return
 *                -EAGAIN if the driver forwarded or consumed the event
 *                itself — the framework then only drops its own ref.
 *                Other negative values are logged and the event is
 *                dropped.  A NULL handle_event behaves like returning 0.
 */
__subsystem struct zstreamer_node_driver_api {
  int (*process)(const struct device *dev, struct net_buf *buf);
  int (*handle_event)(const struct device *dev, struct net_buf *buf);
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

/**
 * @brief Common init for all node types.
 *
 * Sets the dev back-pointer, initialises the fifo, and creates the
 * node thread using the thread_entry stored in config.  Can be used
 * directly as a Zephyr init function, or called at the end of a
 * custom driver init.
 */
extern int zstreamer_node_common_init(const struct device *dev);

/**
 * @brief Thread entry for through-nodes (processors).
 *
 * Declared here so ZSTREAMER_NODE_CONFIG_INIT can reference it.
 */
extern void zstreamer_node_thread_entry(void *p1, void *p2, void *p3);

/**
 * @brief Distribute a buffer to child devices.
 *
 * Honours the copy-on-write / readonly optimisation.
 */
extern void zstreamer_node_distribute(const struct device *dev,
                                      struct net_buf *buf,
                                      const struct device *const *children,
                                      size_t num_children);

/**
 * @brief Drain (unref) all pending buffers from a fifo.
 */
extern void zstreamer_node_drain_fifo(struct k_fifo *fifo);

/**
 * @brief Allocate a zero-length event buffer and distribute it to the
 *        node's children.
 *
 * The event travels through the same fifos as data buffers, so it
 * arrives downstream in order with the data emitted around it.
 *
 * @param dev     Any zstreamer node device.
 * @param type    Event type (must not be ZSTREAMER_BUF_DATA).
 * @param timeout Buffer allocation timeout.
 * @return 0 on success, -ENOMEM if allocation timed out.
 */
int zstreamer_node_send_event(const struct device *dev,
                              enum zstreamer_buf_type type,
                              k_timeout_t timeout);

/**
 * @brief Run a node's handle_event hook and forward the event.
 *
 * Framework helper used by the thread entries: calls the driver's
 * handle_event (if any) and, unless the driver consumed the event,
 * distributes it to the given children.  Consumes the caller's ref.
 */
void zstreamer_node_dispatch_event(const struct device *dev,
                                   struct net_buf *buf,
                                   const struct device *const *children,
                                   size_t num_children);

/**
 * @brief Through-node config initializer.
 *
 * Produces a braced initializer value for struct zstreamer_node_config.
 * Requires ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier in the same translation unit so that the children array symbol
 * (zstreamer_node_children_##inst) is visible.
 *
 * Usage: .common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),
 *
 * @param inst      Devicetree instance number.
 * @param _readonly true if process() never modifies the buffer contents.
 */
#define ZSTREAMER_NODE_CONFIG_INIT(inst, _readonly)                            \
  {Z_ZSTREAMER_NODE_BASE_CONFIG_INIT(inst, zstreamer_node_children_##inst,     \
                                     Z_ZSTREAMER_NUM_CHILDREN(inst),           \
                                     zstreamer_node_thread_entry, _readonly)}

/**
 * @brief Through-node data initializer.
 *
 * Produces a braced initializer value for struct zstreamer_node_data.
 * Requires ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst) to have been called
 * earlier so that the stack symbol (zstreamer_node_stack_##inst) is visible.
 * Zephyr runtime fields (dev, fifo, thread) are initialized later by
 * zstreamer_node_common_init().
 *
 * Usage: .common = ZSTREAMER_NODE_DATA_INIT(inst),
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_NODE_DATA_INIT(inst)                                         \
  {Z_ZSTREAMER_NODE_BASE_DATA_INIT(zstreamer_node, inst)}

/**
 * @brief Pre-define the thread stack and children array for a through-node.
 *
 * Emits the static symbols required by ZSTREAMER_NODE_CONFIG_INIT and
 * ZSTREAMER_NODE_DATA_INIT:
 *   - zstreamer_node_children_<inst>  (device-pointer array)
 *   - zstreamer_node_stack_<inst>     (thread stack)
 *
 * Must be called BEFORE defining the driver's data/config structs so
 * that these symbols are visible to their initialisers.
 *
 * @param inst  Devicetree instance number.
 */
#define ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst)                                \
  Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_node, inst);                           \
  static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                    \
                               ZSTREAMER_THREAD_STACK_SIZE)

/**
 * @}
 */

/** @cond INTERNAL_HIDDEN */

#define Z_ZSTREAMER_NODE_BASE_DATA_INIT(prefix, inst)                          \
  .stack = prefix##_stack_##inst

#define Z_ZSTREAMER_NODE_BASE_CONFIG_INIT(inst, _children, _num_children,      \
                                          _entry, _readonly)                   \
  .graph = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst))),                        \
  .thread_entry = _entry,                                                      \
  .thread_priority = DT_INST_PROP(inst, thread_priority),                      \
  .readonly = _readonly, .children = _children, .num_children = _num_children

#define Z_ZSTREAMER_NODE_CHILD_DEV_GET(node_id, prop, idx)                     \
  DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))

/* clang-format off */
#define Z_ZSTREAMER_CHILDREN_DEFINE(prefix, inst)                              \
  static const struct device *const prefix##_children_##inst[] = {COND_CODE_1( \
      DT_NODE_HAS_PROP(DT_DRV_INST(inst), children),                           \
      (DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), children,                   \
                                Z_ZSTREAMER_NODE_CHILD_DEV_GET, (, ))),        \
      ())}

#define Z_ZSTREAMER_NUM_CHILDREN(inst)                                         \
  COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), children),                   \
              (DT_PROP_LEN(DT_DRV_INST(inst), children)), (0))
/* clang-format on */

/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_NODE_H_ */
