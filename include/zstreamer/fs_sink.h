/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the file system sink zstreamer node driver
 */

#ifndef ZSTREAMER_FS_SINK_H_
#define ZSTREAMER_FS_SINK_H_

#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type for generating filenames.
 *
 * @param dev        The sink-fs device.
 * @param buf        Buffer to write the filename into.
 * @param buf_size   Size of the buffer.
 * @param user_data  User-provided context pointer.
 * @return 0 on success, negative errno on failure.
 */
typedef int (*fs_sink_filename_cb_t)(const struct device *dev, char *buf,
                                     size_t buf_size, void *user_data);

/**
 * @brief Set a custom filename handler for a sink-fs device.
 *
 * Must be called before starting the node. If not called, a default
 * handler is used that generates "<mount_path>/<index>.bin".
 *
 * @param dev        The sink-fs device.
 * @param cb         Filename callback function.
 * @param user_data  Opaque pointer passed to the callback.
 * @return 0 on success, negative errno on failure.
 */
int fs_sink_set_filename_handler(const struct device *dev,
                                 fs_sink_filename_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_FS_SINK_H_ */
