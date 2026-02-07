/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal types for the zstreamer graph device
 */

#ifndef ZSTREAMER_ZSTREAMER_H_
#define ZSTREAMER_ZSTREAMER_H_

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Graph device configuration (populated by DT macros). */
struct zstreamer_graph_config {
	struct net_buf_pool *pool;
};

/** Graph device runtime data. */
struct zstreamer_graph_data {
	atomic_t started;
};

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_ZSTREAMER_H_ */
