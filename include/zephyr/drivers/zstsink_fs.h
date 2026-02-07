/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the file system sink zstnode driver
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_ZSTSINK_FS_H_
#define ZEPHYR_INCLUDE_DRIVERS_ZSTSINK_FS_H_

#include <zephyr/device.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type for generating filenames.
 *
 * @param dev        The zstsink-fs device.
 * @param buf        Buffer to write the filename into.
 * @param buf_size   Size of the buffer.
 * @param user_data  User-provided context pointer.
 * @return 0 on success, negative errno on failure.
 */
typedef int (*zstsink_fs_filename_cb_t)(const struct device *dev,
					char *buf, size_t buf_size,
					void *user_data);

/**
 * @brief Set a custom filename handler for a zstsink-fs device.
 *
 * Must be called before starting the node. If not called, a default
 * handler is used that generates "<mount_path>/<index>.bin".
 *
 * @param dev        The zstsink-fs device.
 * @param cb         Filename callback function.
 * @param user_data  Opaque pointer passed to the callback.
 * @return 0 on success, negative errno on failure.
 */
int zstsink_fs_set_filename_handler(const struct device *dev,
				    zstsink_fs_filename_cb_t cb,
				    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_ZSTSINK_FS_H_ */
